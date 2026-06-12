/// The rig editor: a full work-area main tab (see App.tsx / openRigEditorTab) that previews a rigged
/// model outside the authored scene. The engine spawns the rig into an isolated preview scene and
/// publishes it through the one viewport subsurface (glued into the center pane here); the side panels
/// show the skeleton tree (left) and clip list + details (right), and the bottom strip is the timeline.
///
/// Lifecycle is keyed to the mount: App renders this with key={rigMeshId}, so switching to a different
/// rig remounts (cleanup exits rig A, mount enters rig B) — an activeKind-only effect would keep
/// previewing A under B's panels. enter-rig-preview / exit-rig-preview stash + restore the camera
/// engine-side, so orbiting never dirties the saved editorCamera.
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Axis3d, Bone, Grid2x2 } from "lucide-react";
import { client } from "../control/client";
import { makeCoalescer } from "../control/coalesce";
import { useSubsurfaceBounds } from "../lib/useSubsurfaceBounds";
import { errorText, notifyError } from "../lib/flash";
import { Button } from "@/components/ui/button";
import { ResizableHandle, ResizablePanel, ResizablePanelGroup } from "@/components/ui/resizable";
import { RigSkeletonTree } from "./RigSkeletonTree";
import { RigClipList } from "./RigClipList";
import { TimelineTransport } from "../components/timeline/TimelineTransport";
import { TimelineSurface } from "../components/timeline/TimelineSurface";
import type { TimelineTarget } from "../components/timeline/shared";
import { useEditorStore } from "../state/store";
import type { RigResult } from "../protocol";

/// Orbit drag sensitivity (degrees of yaw/pitch per CSS pixel) and zoom factor per wheel notch.
const ORBIT_SENS_DEG_PER_PX = 0.4;
const ZOOM_PER_WHEEL = 1.1;

interface OrbitState {
  target: { x: number; y: number; z: number };
  distance: number;
  yaw: number;
  pitch: number;
}

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

export function RigEditorWorkspace({ rigMeshId }: { rigMeshId: string }) {
  const hostRef = useRef<HTMLDivElement | null>(null);
  const [status, setStatus] = useState<"loading" | "ready" | "error">("loading");
  const [errorMessage, setErrorMessage] = useState("");
  const [rig, setRig] = useState<RigResult | null>(null);
  const [rigEntity, setRigEntity] = useState<string | null>(null);
  const [floor, setFloor] = useState(true);
  // Overlay toggles default on (the engine forces show=on while previewing); local mirror for the chips.
  const [showBones, setShowBones] = useState(true);
  const [showAxes, setShowAxes] = useState(false);
  // The bone the tree has highlighted (a get-rig node index); local view state, not scene selection.
  const [highlightJoint, setHighlightJoint] = useState(-1);

  const orbit = useRef<OrbitState>({
    target: { x: 0, y: 0, z: 0 },
    distance: 5,
    yaw: -37,
    pitch: -29,
  });

  // Glue the single subsurface into this pane while the rig tab is active (the dock host is parked).
  useSubsurfaceBounds(hostRef);

  // One coalesced set-camera in flight at a time (the serialized wire — never one call per drag tick).
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

  // Enter the preview on mount, exit on unmount. Remounting (a different rigMeshId) runs cleanup first,
  // so a rig A -> rig B switch is a real exit/enter. Errors land the workspace in its error state.
  useEffect(() => {
    let cancelled = false;
    void (async () => {
      try {
        const entered = await client.enterRigPreview(rigMeshId);
        if (cancelled) {
          return;
        }
        setRigEntity(entered.rigEntity);
        const cam = await client.getCamera();
        orbit.current = {
          target: entered.target,
          distance: entered.distance,
          yaw: cam.yaw,
          pitch: cam.pitch,
        };
        const loaded = await client.getRig(rigMeshId);
        if (cancelled) {
          return;
        }
        setRig(loaded);
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
      void client.exitRigPreview().catch(() => {});
    };
  }, [rigMeshId]);

  const onBoneSelect = useCallback((joint: number) => {
    setHighlightJoint(joint);
    void client.setSkeletonHighlight(joint).catch((err: unknown) => notifyError(errorText(err)));
  }, []);

  // The preview rig is the engine selection, so the store's selection-keyed animationState mirrors it;
  // the timeline targets the spawned rig entity. The clip list panel owns clip picking, so the
  // transport hides its clip Select. Enabled once the preview is entered.
  const animationState = useEditorStore((s) => s.animationState);
  const timelineTarget: TimelineTarget = {
    entityId: rigEntity,
    state: animationState,
    clips: [],
    enabled: status === "ready" && rigEntity !== null,
  };

  // Space = play/pause while the rig tab is focused (not while a text field is). The workspace mounts
  // only when the tab is active, so a window listener is scoped correctly.
  useEffect(() => {
    const onKey = (e: KeyboardEvent): void => {
      if (e.code !== "Space" || !rigEntity) {
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
        void client.pauseAnimation(rigEntity).catch((err: unknown) => notifyError(errorText(err)));
      } else if (st?.clip) {
        void client
          .playAnimation(rigEntity, String(st.clip), { loop: st.wrap !== "once" })
          .catch((err: unknown) => notifyError(errorText(err)));
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [rigEntity]);

  const toggleFloor = useCallback(() => {
    setFloor((prev) => {
      const next = !prev;
      void client
        .setRigPreviewOptions({ floor: next })
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

  // Orbit: left-drag rotates the camera around the framed target, wheel dollies. Both reconstruct the
  // eye from the orbit state and push a coalesced set-camera; exit-rig-preview restores the stash so
  // this never dirties the saved editorCamera.
  const dragging = useRef(false);
  const lastPointer = useRef({ x: 0, y: 0 });
  const onPointerDown = useCallback((e: React.PointerEvent<HTMLDivElement>) => {
    if (e.button !== 0) {
      return;
    }
    dragging.current = true;
    lastPointer.current = { x: e.clientX, y: e.clientY };
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
      const o = orbit.current;
      o.yaw += dx * ORBIT_SENS_DEG_PER_PX;
      o.pitch = Math.max(-89, Math.min(89, o.pitch - dy * ORBIT_SENS_DEG_PER_PX));
      cameraCoalescer.push({ ...o });
    },
    [cameraCoalescer],
  );
  const onPointerUp = useCallback((e: React.PointerEvent<HTMLDivElement>) => {
    dragging.current = false;
    if (e.currentTarget.hasPointerCapture(e.pointerId)) {
      e.currentTarget.releasePointerCapture(e.pointerId);
    }
  }, []);
  const onWheel = useCallback(
    (e: React.WheelEvent<HTMLDivElement>) => {
      const o = orbit.current;
      o.distance = Math.max(0.2, o.distance * (e.deltaY > 0 ? ZOOM_PER_WHEEL : 1 / ZOOM_PER_WHEEL));
      cameraCoalescer.push({ ...o });
    },
    [cameraCoalescer],
  );

  if (status === "error") {
    return (
      <main className="flex min-h-0 flex-1 flex-col items-center justify-center gap-2 bg-background px-6 text-center">
        <Bone className="size-8 text-muted-foreground" />
        <p className="text-sm text-foreground">This asset has no rig.</p>
        <p className="max-w-md text-xs text-muted-foreground">{errorMessage}</p>
        <p className="text-xs text-muted-foreground">
          Re-import the model as a rigged asset to preview it.
        </p>
      </main>
    );
  }

  return (
    <main className="flex min-h-0 flex-1 flex-col overflow-hidden bg-background">
      <div className="flex items-center gap-3 border-b border-border px-3 py-2">
        <Bone className="size-4 text-muted-foreground" />
        <span className="text-sm font-medium text-foreground">{rig?.name ?? "Rig"}</span>
        <span className="text-xs text-muted-foreground">
          {status === "loading"
            ? "Loading…"
            : `${rig?.bones.length ?? 0} bones · ${rig?.clips.length ?? 0} clips`}
        </span>
        <div className="ml-auto flex items-center gap-1">
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
          <Button
            variant={floor ? "secondary" : "ghost"}
            size="icon-sm"
            onClick={toggleFloor}
            aria-label="Toggle preview floor"
          >
            <Grid2x2 className="size-4" />
          </Button>
        </div>
      </div>
      <ResizablePanelGroup orientation="horizontal" className="min-h-0 flex-1">
        <ResizablePanel defaultSize={18} minSize={12}>
          <RigSkeletonTree
            bones={rig?.bones ?? []}
            selectedIndex={highlightJoint}
            onSelect={onBoneSelect}
          />
        </ResizablePanel>
        <ResizableHandle />
        <ResizablePanel defaultSize={60} minSize={30}>
          {/* The transparent hole down to the engine's subsurface (the preview scene renders here). */}
          <div
            className="relative h-full w-full overflow-hidden"
            onPointerDown={onPointerDown}
            onPointerMove={onPointerMove}
            onPointerUp={onPointerUp}
            onPointerCancel={onPointerUp}
            onWheel={onWheel}
          >
            <div ref={hostRef} className="viewport-host" />
          </div>
        </ResizablePanel>
        <ResizableHandle />
        <ResizablePanel defaultSize={22} minSize={14}>
          <RigClipList rig={rig} rigEntity={rigEntity} />
        </ResizablePanel>
      </ResizablePanelGroup>
      {/* Bottom timeline strip: the shared transport + surface (phase 8), targeted at the preview rig. */}
      <div className="flex h-[200px] flex-none flex-col border-t border-border bg-background">
        <TimelineTransport target={timelineTarget} showClipSelect={false} />
        <TimelineSurface target={timelineTarget} />
      </div>
    </main>
  );
}
