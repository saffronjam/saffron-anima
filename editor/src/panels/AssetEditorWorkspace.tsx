/// The asset editor: a full work-area main tab (see App.tsx / openAssetEditorTab) that previews ANY
/// model — rigged or static — outside the authored scene. The engine spawns the model into an isolated
/// preview scene and publishes it through the one viewport subsurface (glued into the center pane here).
/// The side panels appear by capability: the skeleton tree (left) only for a rigged model, the clip list
/// + details (right) and the bottom timeline only when the model has clips. A static model is just the
/// framed viewport (orbit, materials, floor) — no rig chrome.
///
/// Orbit is eased: input moves a target, a rAF loop drains current→target with the engine's tau (refs
/// only, no React re-render), so a slight lag reads as smooth motion. Loading is masked: the panels +
/// subsurface mount only once the model's capabilities are known (so the first frame already has the
/// right panels at the final width), behind a "Preparing…" spinner that lifts after the viewport settles.
///
/// Lifecycle is keyed to the mount: App renders this with key={assetId}, so switching to a different
/// model remounts (cleanup exits model A, mount enters model B) — an activeKind-only effect would keep
/// previewing A under B's panels. enter-asset-preview / exit-asset-preview stash + restore the camera
/// engine-side, so orbiting never dirties the saved editorCamera.
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Axis3d, Bone, Box, Grid2x2 } from "lucide-react";
import { client } from "../control/client";
import { makeCoalescer } from "../control/coalesce";
import { useSubsurfaceBounds } from "../lib/useSubsurfaceBounds";
import { errorText, notifyError } from "../lib/flash";
import { Button } from "@/components/ui/button";
import { DockRoot } from "@/components/dock/DockRoot";
import { DockPanelsHost } from "@/components/dock/DockPanelsHost";
import {
  AssetPreviewProvider,
  Preparing,
  type AssetPreviewContextValue,
} from "./assetEditorPanels";
import { useEditorStore } from "../state/store";
import type { AssetModelResult } from "../protocol";

/// Orbit drag sensitivity (degrees of yaw/pitch per CSS pixel) and zoom factor per wheel notch.
const ORBIT_SENS_DEG_PER_PX = 0.4;
const ZOOM_PER_WHEEL = 1.1;
/// Orbit easing: drain current→target each frame at this time constant (mirrors the engine's tau=0.025
/// gizmo/edit smoothing); stop when within the epsilons. The distance/target epsilon is a fraction of
/// the live distance (an absolute epsilon would never settle a tiny model and would over-shoot a large one).
const ORBIT_TAU_S = 0.025;
const ORBIT_EPS_DEG = 0.01;
const ORBIT_EPS_DIST_FRAC = 0.0005;
/// Zoom bounds relative to the engine-framed distance, so a small model can be dollied in close (the old
/// fixed 0.2 floor exceeded a small model's framed distance) while a ceiling stops it flying away.
const ZOOM_MIN_FRAC = 0.02;
const ZOOM_MAX_FRAC = 8;
const ZOOM_MIN_ABS = 0.001;
/// A pointer-up within this many pixels of pointer-down is a click (joint pick), not an orbit drag.
const CLICK_SLOP_PX = 4;

interface OrbitState {
  target: { x: number; y: number; z: number };
  distance: number;
  yaw: number;
  pitch: number;
}

const INITIAL_ORBIT: OrbitState = {
  target: { x: 0, y: 0, z: 0 },
  distance: 5,
  yaw: -37,
  pitch: -29,
};

/// The engine's fly-cam forward basis from yaw/pitch (mirrors sceneEditCameraForward), so the editor's
/// orbit reconstructs the eye as target - forward * distance.
function forwardFromYawPitch(
  yawDeg: number,
  pitchDeg: number,
): { x: number; y: number; z: number } {
  const yaw = (yawDeg * Math.PI) / 180;
  const pitch = (pitchDeg * Math.PI) / 180;
  return {
    x: Math.cos(pitch) * Math.sin(yaw),
    y: Math.sin(pitch),
    z: -Math.cos(pitch) * Math.cos(yaw),
  };
}

function cloneOrbit(o: OrbitState): OrbitState {
  return { target: { ...o.target }, distance: o.distance, yaw: o.yaw, pitch: o.pitch };
}

export function AssetEditorWorkspace({ assetId, active }: { assetId: string; active: boolean }) {
  const hostRef = useRef<HTMLDivElement | null>(null);
  const [status, setStatus] = useState<"loading" | "ready" | "error">("loading");
  const [errorMessage, setErrorMessage] = useState("");
  const [model, setModel] = useState<AssetModelResult | null>(null);
  const [rootEntity, setRootEntity] = useState<string | null>(null);
  // The viewport has resized + presented at the final pane size (masks the one resize behind the spinner).
  const [viewportSettled, setViewportSettled] = useState(false);
  const [floor, setFloor] = useState(true);
  // Overlay toggles default on (the engine forces show=on while previewing); local mirror for the chips.
  const [showBones, setShowBones] = useState(true);
  const [showAxes, setShowAxes] = useState(false);
  // The bone the tree has highlighted (a get-asset-model node index); local view state, not selection.
  const [highlightJoint, setHighlightJoint] = useState(-1);

  // What the model can do gates the panels: the skeleton tree only for a rigged model, the clip list +
  // timeline only when it has clips. A static model shows just the viewport + floor toggle.
  const caps = model?.capabilities ?? null;
  const hasRig = caps?.hasRig ?? false;
  const hasClips = (caps?.clipCount ?? 0) > 0;
  const ready = status === "ready";

  // Orbit: the input target and the eased current, both refs (the rAF loop must not re-render). framed
  // distance seeds the zoom bounds so they scale to the model.
  const targetOrbit = useRef<OrbitState>(cloneOrbit(INITIAL_ORBIT));
  const currentOrbit = useRef<OrbitState>(cloneOrbit(INITIAL_ORBIT));
  const framedDistance = useRef(INITIAL_ORBIT.distance);
  const rafId = useRef<number | null>(null);
  const lastFrameTs = useRef(0);

  // Glue the single subsurface into this pane while this tab is ACTIVE and the layout is final (the host
  // mounts with the correct panels), and lift the spinner after the first settled resize presents. When
  // inactive (the tab is parked, preview suspended) the hook disables so the scene viewport drives the
  // subsurface; re-activating re-runs the hook (a fresh resize + mask) for the resumed preview.
  const onViewportSettled = useCallback(() => setViewportSettled(true), []);
  useSubsurfaceBounds(hostRef, { enabled: active && ready, onSettled: onViewportSettled });

  // One coalesced set-camera in flight at a time (the serialized wire — never one call per frame tick).
  const cameraCoalescer = useMemo(
    () =>
      makeCoalescer<OrbitState>({
        throttleMs: 16,
        send: (o) => {
          const f = forwardFromYawPitch(o.yaw, o.pitch);
          return client
            .setCamera({
              position: {
                x: o.target.x - f.x * o.distance,
                y: o.target.y - f.y * o.distance,
                z: o.target.z - f.z * o.distance,
              },
              yaw: o.yaw,
              pitch: o.pitch,
            })
            .then(() => {});
        },
      }),
    [],
  );

  // Ease current→target one frame, push the eased camera, and either re-arm or settle. Refs only.
  const tickOrbit = useCallback(() => {
    const now = performance.now();
    const dt = lastFrameTs.current ? (now - lastFrameTs.current) / 1000 : 0;
    lastFrameTs.current = now;
    const t = targetOrbit.current;
    const c = currentOrbit.current;
    const alpha = 1 - Math.exp(-dt / ORBIT_TAU_S);
    c.yaw += (t.yaw - c.yaw) * alpha;
    c.pitch += (t.pitch - c.pitch) * alpha;
    c.distance += (t.distance - c.distance) * alpha;
    c.target.x += (t.target.x - c.target.x) * alpha;
    c.target.y += (t.target.y - c.target.y) * alpha;
    c.target.z += (t.target.z - c.target.z) * alpha;
    cameraCoalescer.push(cloneOrbit(c));

    const distEps = ORBIT_EPS_DIST_FRAC * Math.max(c.distance, 1e-4);
    const settled =
      Math.abs(t.yaw - c.yaw) < ORBIT_EPS_DEG &&
      Math.abs(t.pitch - c.pitch) < ORBIT_EPS_DEG &&
      Math.abs(t.distance - c.distance) < distEps &&
      Math.abs(t.target.x - c.target.x) < distEps &&
      Math.abs(t.target.y - c.target.y) < distEps &&
      Math.abs(t.target.z - c.target.z) < distEps;
    if (settled) {
      currentOrbit.current = cloneOrbit(t);
      cameraCoalescer.push(cloneOrbit(t)); // land exactly on the target
      rafId.current = null;
      lastFrameTs.current = 0;
      return;
    }
    rafId.current = requestAnimationFrame(tickOrbit);
  }, [cameraCoalescer]);

  const ensureOrbitLoop = useCallback(() => {
    if (rafId.current === null) {
      lastFrameTs.current = 0;
      rafId.current = requestAnimationFrame(tickOrbit);
    }
  }, [tickOrbit]);

  // Enter the preview on mount, exit on unmount. Remounting (a different assetId) runs cleanup first,
  // so a model A -> model B switch is a real exit/enter. A real failure lands the workspace in its
  // error state; a static (skinless) model is NOT a failure — it opens with an empty bone tree. The
  // framed camera SNAPS (target == current, no ease on first show).
  useEffect(() => {
    let cancelled = false;
    void (async () => {
      try {
        const entered = await client.enterAssetPreview(assetId);
        if (cancelled) {
          return;
        }
        setRootEntity(entered.rootEntity);
        const cam = await client.getCamera();
        if (cancelled) {
          return;
        }
        const framed: OrbitState = {
          target: { ...entered.target },
          distance: entered.distance,
          yaw: cam.yaw,
          pitch: cam.pitch,
        };
        targetOrbit.current = cloneOrbit(framed);
        currentOrbit.current = cloneOrbit(framed);
        framedDistance.current = entered.distance;
        const loaded = await client.getAssetModel(assetId);
        if (cancelled) {
          return;
        }
        setModel(loaded);
        setStatus("ready");
      } catch (err) {
        if (!cancelled) {
          setErrorMessage(errorText(err));
          setStatus("error");
        }
      }
    })();
    return () => {
      cancelled = true;
      if (rafId.current !== null) {
        cancelAnimationFrame(rafId.current);
        rafId.current = null;
      }
      void client.exitAssetPreview().catch(() => {});
    };
  }, [assetId]);

  // Suspend/resume the preview as this tab loses/gains focus (the mount enters, the unmount exits). The
  // engine keeps the preview scene alive across a suspend, so resuming is instant (no re-spawn) and the
  // orbit camera is preserved. The first run is the mount itself (already entered) — skip it.
  const firstActiveRun = useRef(true);
  useEffect(() => {
    if (firstActiveRun.current) {
      firstActiveRun.current = false;
      return;
    }
    if (active) {
      setViewportSettled(false); // re-mask while the resumed preview resizes back into this pane
      void client.resumeAssetPreview().catch((err: unknown) => notifyError(errorText(err)));
    } else {
      void client.suspendAssetPreview().catch((err: unknown) => notifyError(errorText(err)));
    }
  }, [active]);

  const onBoneSelect = useCallback((joint: number) => {
    setHighlightJoint(joint);
    void client.setSkeletonHighlight(joint).catch((err: unknown) => notifyError(errorText(err)));
  }, []);

  // Capability gating through the dock model, not a render branch (NO-COMPAT): once the
  // model's capabilities are known, open the panels it supports (rig → skeleton; clips →
  // clips + the timeline) and close the rest. Their leaves are persistent, so DockRoot
  // collapses an empty one — a static model shows just the preview. `preview` is always open.
  useEffect(() => {
    if (!ready) {
      return;
    }
    const { openPanel, closePanel } = useEditorStore.getState();
    if (hasRig) {
      openPanel("skeleton");
    } else {
      closePanel("skeleton");
    }
    if (hasClips) {
      openPanel("clips");
      openPanel("assetTimeline");
    } else {
      closePanel("clips");
      closePanel("assetTimeline");
    }
  }, [ready, hasRig, hasClips]);

  // Space = play/pause while THIS tab is active (the workspace stays mounted-but-hidden when parked, so
  // the window listener must no-op unless active) and no text field is focused.
  useEffect(() => {
    const onKey = (e: KeyboardEvent): void => {
      if (!active || e.code !== "Space" || !rootEntity) {
        return;
      }
      const el = document.activeElement;
      if (
        el instanceof HTMLElement &&
        el.closest("input, textarea, select, [contenteditable='true']")
      ) {
        return;
      }
      e.preventDefault();
      const st = useEditorStore.getState().animationState;
      if (st?.playing) {
        void client.pauseAnimation(rootEntity).catch((err: unknown) => notifyError(errorText(err)));
      } else if (st?.clip) {
        void client
          .playAnimation(rootEntity, String(st.clip), { loop: st.wrap !== "once" })
          .catch((err: unknown) => notifyError(errorText(err)));
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [active, rootEntity]);

  const toggleFloor = useCallback(() => {
    setFloor((prev) => {
      const next = !prev;
      void client
        .setAssetPreviewOptions({ floor: next })
        .catch((err: unknown) => notifyError(errorText(err)));
      return next;
    });
  }, []);

  const toggleBones = useCallback(() => {
    setShowBones((prev) => {
      const next = !prev;
      void client
        .setSkeletonOverlay({ show: next })
        .catch((err: unknown) => notifyError(errorText(err)));
      return next;
    });
  }, []);

  const toggleAxes = useCallback(() => {
    setShowAxes((prev) => {
      const next = !prev;
      void client
        .setSkeletonOverlay({ axes: next })
        .catch((err: unknown) => notifyError(errorText(err)));
      return next;
    });
  }, []);

  // Orbit: left-drag rotates the camera around the framed target, wheel dollies. Both write the orbit
  // TARGET and arm the ease loop; exit-asset-preview restores the stash so this never dirties the saved
  // editorCamera. A click (no drag) on a rigged model picks the nearest joint and selects it in the tree.
  const dragging = useRef(false);
  const lastPointer = useRef({ x: 0, y: 0 });
  const downPos = useRef({ x: 0, y: 0 });
  const onPointerDown = useCallback((e: React.PointerEvent<HTMLDivElement>) => {
    if (e.button !== 0) {
      return;
    }
    dragging.current = true;
    lastPointer.current = { x: e.clientX, y: e.clientY };
    downPos.current = { x: e.clientX, y: e.clientY };
    e.currentTarget.setPointerCapture(e.pointerId);
  }, []);
  const onPointerMove = useCallback(
    (e: React.PointerEvent<HTMLDivElement>) => {
      if (!dragging.current) {
        return;
      }
      const dx = e.clientX - lastPointer.current.x;
      const dy = e.clientY - lastPointer.current.y;
      lastPointer.current = { x: e.clientX, y: e.clientY };
      const o = targetOrbit.current;
      o.yaw += dx * ORBIT_SENS_DEG_PER_PX;
      o.pitch = Math.max(-89, Math.min(89, o.pitch - dy * ORBIT_SENS_DEG_PER_PX));
      ensureOrbitLoop();
    },
    [ensureOrbitLoop],
  );
  const onPointerUp = useCallback(
    (e: React.PointerEvent<HTMLDivElement>) => {
      dragging.current = false;
      if (e.currentTarget.hasPointerCapture(e.pointerId)) {
        e.currentTarget.releasePointerCapture(e.pointerId);
      }
      // A click (negligible movement) on a rigged model picks the nearest joint and selects its bone.
      if (
        !hasRig ||
        Math.hypot(e.clientX - downPos.current.x, e.clientY - downPos.current.y) >= CLICK_SLOP_PX
      ) {
        return;
      }
      const rect = e.currentTarget.getBoundingClientRect();
      if (rect.width <= 0 || rect.height <= 0) {
        return;
      }
      const u = Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width));
      const v = Math.min(1, Math.max(0, (e.clientY - rect.top) / rect.height));
      void client
        .pickSkeletonJoint(u, v)
        .then((res) => {
          if (res.found && res.nodeIndex >= 0) {
            onBoneSelect(res.nodeIndex);
          }
        })
        .catch((err: unknown) => notifyError(errorText(err)));
    },
    [hasRig, onBoneSelect],
  );
  const onWheel = useCallback(
    (e: React.WheelEvent<HTMLDivElement>) => {
      const o = targetOrbit.current;
      const minD = Math.max(ZOOM_MIN_ABS, framedDistance.current * ZOOM_MIN_FRAC);
      const maxD = framedDistance.current * ZOOM_MAX_FRAC;
      const next = o.distance * (e.deltaY > 0 ? ZOOM_PER_WHEEL : 1 / ZOOM_PER_WHEEL);
      o.distance = Math.min(maxD, Math.max(minD, next));
      ensureOrbitLoop();
    },
    [ensureOrbitLoop],
  );

  // The live preview state the dock panels read. Provided around this island's DockRoot +
  // DockPanelsHost so the portaled panel bodies inherit it across the leaves they land in.
  const previewContext: AssetPreviewContextValue = useMemo(
    () => ({
      model,
      rootEntity,
      highlightJoint,
      onBoneSelect,
      hostRef,
      viewportSettled,
      orbit: { onPointerDown, onPointerMove, onPointerUp, onWheel },
      active,
      ready,
    }),
    [
      model,
      rootEntity,
      highlightJoint,
      onBoneSelect,
      viewportSettled,
      onPointerDown,
      onPointerMove,
      onPointerUp,
      onWheel,
      active,
      ready,
    ],
  );

  if (status === "error") {
    return (
      <main className="flex min-h-0 flex-1 flex-col items-center justify-center gap-2 bg-background px-6 text-center">
        <Box className="size-8 text-muted-foreground" />
        <p className="text-sm text-foreground">Could not open this asset.</p>
        <p className="max-w-md text-xs text-muted-foreground">{errorMessage}</p>
        <p className="text-xs text-muted-foreground">
          Re-import the model if the problem persists.
        </p>
      </main>
    );
  }

  // No bg on <main>: the center preview pane must stay a transparent hole down to the engine's Wayland
  // subsurface (composited below the webview). Every other region paints its own opaque bg-background
  // (the toolbar below, the side panels, the timeline strip, the Preparing spinner), so only the pane
  // shows through. The panels + subsurface mount only once `ready` (capabilities known) so the first
  // subsurface frame is already at the final pane width — no panel pop-in, no resize stretch.
  return (
    <main className="flex min-h-0 flex-1 flex-col overflow-hidden">
      <div className="flex items-center gap-3 border-b border-border bg-background px-3 py-2">
        <Box className="size-4 text-muted-foreground" />
        <span className="text-sm font-medium text-foreground">{model?.name ?? "Asset"}</span>
        {ready ? (
          <div className="ml-auto flex items-center gap-1">
            {hasRig ? (
              <>
                <Button
                  variant={showBones ? "secondary" : "ghost"}
                  size="icon-sm"
                  onClick={toggleBones}
                  aria-label="Toggle skeleton overlay"
                >
                  <Bone className="size-4" />
                </Button>
                <Button
                  variant={showAxes ? "secondary" : "ghost"}
                  size="icon-sm"
                  onClick={toggleAxes}
                  aria-label="Toggle joint axes"
                >
                  <Axis3d className="size-4" />
                </Button>
              </>
            ) : null}
            <Button
              variant={floor ? "secondary" : "ghost"}
              size="icon-sm"
              onClick={toggleFloor}
              aria-label="Toggle preview floor"
            >
              <Grid2x2 className="size-4" />
            </Button>
          </div>
        ) : null}
      </div>
      {ready ? (
        // The asset-editor dock island: its skeleton / preview(locked) / clips / assetTimeline
        // panels are a draggable DockRoot tree (the second dockspace kind). The panels render
        // through this island's own DockPanelsHost, portaled into the leaves — so they inherit
        // the preview state via AssetPreviewProvider and survive retab/split moves.
        <AssetPreviewProvider value={previewContext}>
          <DockPanelsHost space="assetEditor" />
          <div className="relative min-h-0 flex-1">
            <DockRoot space="assetEditor" />
          </div>
        </AssetPreviewProvider>
      ) : (
        <Preparing className="flex min-h-0 flex-1 items-center justify-center bg-background" />
      )}
    </main>
  );
}
