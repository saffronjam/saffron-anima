/// The viewport region: a transparent div the engine's render shows through (the
/// presenter holds a wayland subsurface glued to this rect, below the webview). It
/// never renders pixels — it owns the screen rectangle and forwards pointer input to
/// the engine over the control plane. A <LoadingOverlay/> sibling covers the region
/// while the renderer is not yet ready.
import { useCallback, useEffect, useLayoutEffect, useMemo, useRef } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { client, type ViewportBounds } from "../control/client";
import { makeCoalescer } from "../control/coalesce";
import { useEditorStore } from "../state/store";
import { LoadingOverlay } from "../app/LoadingOverlay";
import { onLayoutSettled } from "../app/layoutBus";
import { bindingFor } from "../lib/keybindings";

/// Resize-end commit debounce: after the last layout/resize change settles, send one
/// final exact bounds — this is the tier that also resizes the engine's render target
/// (expensive offscreen recreation, so never on live drag ticks).
const RESIZE_END_DEBOUNCE_MS = 150;

/// Live-sync throttle during a drag: the subsurface geometry (position + stretched
/// destination) tracks the host div at this cadence; the engine keeps rendering at the
/// old size until the end commit.
const LIVE_SYNC_THROTTLE_MS = 16;

/// Pointer travel (CSS px) below which a press-release is treated as a click
/// (ray-pick) rather than a gizmo drag.
const DRAG_THRESHOLD_PX = 3;

/// Throttle for streamed fly-cam input while pointer lock is held, in milliseconds.
/// Look deltas accumulate between sends, so nothing is lost to the throttle.
const FLY_STREAM_MS = 16;

/// Throttle for streamed gizmo pointer phases (hover/drag), in milliseconds.
const GIZMO_STREAM_MS = 16;

/// Read the div's logical CSS rect + the window scale factor. Rust positions the
/// subsurface in logical units and derives the engine's device-pixel render size.
async function computeBounds(el: HTMLElement): Promise<ViewportBounds | null> {
  const rect = el.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) {
    return null;
  }
  const scale = await getCurrentWindow().scaleFactor();
  return {
    x: Math.round(rect.x),
    y: Math.round(rect.y),
    width: Math.round(rect.width),
    height: Math.round(rect.height),
    scale,
  };
}

function boundsEqual(a: ViewportBounds | null, b: ViewportBounds): boolean {
  return (
    a !== null &&
    a.x === b.x &&
    a.y === b.y &&
    a.width === b.width &&
    a.height === b.height &&
    a.scale === b.scale
  );
}

/// Normalized [0,1] viewport coordinate, (0,0) = top-left.
interface Uv {
  u: number;
  v: number;
}

/// Map a pointer event to {u,v} in [0,1] using the panel's own client rect.
function eventToUv(el: HTMLElement, event: PointerEvent): Uv {
  const rect = el.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) {
    return { u: 0, v: 0 };
  }
  const u = (event.clientX - rect.left) / rect.width;
  const v = (event.clientY - rect.top) / rect.height;
  return {
    u: Math.min(1, Math.max(0, u)),
    v: Math.min(1, Math.max(0, v)),
  };
}

export function ViewportPanel() {
  const hostRef = useRef<HTMLDivElement | null>(null);
  const attachedRef = useRef(false);
  const setPhase = useEditorStore((s) => s.setPhase);
  const setSelectedId = useEditorStore((s) => s.setSelectedId);
  const setDragActive = useEditorStore((s) => s.setDragActive);
  const viewportHidden = useEditorStore((s) => s.viewportHidden);

  // Coalescers stream the hover and drag phases to the engine at >= GIZMO_STREAM_MS
  // apart, buffering only the latest NDC so a burst of pointermove collapses to one
  // in-flight call. Stable across renders.
  const hoverCoalescer = useMemo(
    () =>
      makeCoalescer<Uv>({
        throttleMs: GIZMO_STREAM_MS,
        send: ({ u, v }) => client.gizmoPointer("hover", u * 2 - 1, v * 2 - 1),
      }),
    [],
  );
  const dragCoalescer = useMemo(
    () =>
      makeCoalescer<Uv>({
        throttleMs: GIZMO_STREAM_MS,
        send: ({ u, v }) => client.gizmoPointer("drag", u * 2 - 1, v * 2 - 1),
      }),
    [],
  );

  // Optimistic post-pick selection: a hit sets store.selectedId immediately so the
  // UI does not wait a full reconcile interval. Empty space deselects.
  const runPick = useCallback(
    async ({ u, v }: Uv): Promise<void> => {
      try {
        const result = await client.pick(u, v);
        if (result.hit && result.id) {
          setSelectedId(result.id);
        } else {
          setSelectedId(null);
        }
      } catch {
        // The engine may be briefly busy; the reconcile poll recovers selection.
      }
    },
    [setSelectedId],
  );

  // Readiness: probe the control plane until the engine has booted + bound its
  // socket, then flip the phase. The `engine-phase` events are emitted from the Rust
  // `.setup()` hook BEFORE this webview registers its listener (Tauri does not buffer
  // pre-listen events), so the probe — not the event — is the gate. (`cancelled`
  // makes any pending retry a no-op after unmount.)
  useLayoutEffect(() => {
    let cancelled = false;

    const probe = async (): Promise<void> => {
      if (cancelled || attachedRef.current) {
        return;
      }
      try {
        await client.viewportNativeInfo();
      } catch {
        if (cancelled) {
          return;
        }
        setTimeout(() => void probe(), 150);
        return;
      }
      if (cancelled) {
        return;
      }
      attachedRef.current = true;
      setPhase("ready");
    };

    void probe();

    return () => {
      cancelled = true;
    };
  }, [setPhase]);

  // Bounds-sync: keep the subsurface glued on resize / dock split changes. Two tiers:
  //   - a THROTTLED live sync (~50ms) on every host-div geometry change (the
  //     ResizeObserver fires during a drag) so the subsurface roughly tracks;
  //   - a single DEBOUNCED resize-end commit (~150ms) so the final bounds land
  //     exactly even if the throttle dropped the last drag frame.
  // The layout bus (Layout's onLayoutChanged) drives the same resize-end commit so a
  // panel-split that settles without further div mutation still commits.
  useEffect(() => {
    const el = hostRef.current;
    if (!el) {
      return;
    }
    let cancelled = false;
    let lastSent: ViewportBounds | null = null;
    let debounceTimer: ReturnType<typeof setTimeout> | null = null;
    let lastLiveSent = 0;
    let liveTimer: ReturnType<typeof setTimeout> | null = null;

    // Live tier moves/stretches the subsurface only; the end tier (resizeEngine) also
    // commits the device-pixel render size so the engine re-renders sharp at the final
    // rect — once per gesture instead of per drag tick.
    const commit = async (resizeEngine: boolean, force = false): Promise<void> => {
      if (cancelled) {
        return;
      }
      const bounds = await computeBounds(el);
      if (cancelled || !bounds) {
        return;
      }
      if (!force && !resizeEngine && boundsEqual(lastSent, bounds)) {
        return;
      }
      lastSent = bounds;
      try {
        await client.setViewportBounds(bounds, resizeEngine);
      } catch {
        // The bridge may be briefly unavailable; the next sync recovers.
      }
    };

    const scheduleEndCommit = (force = false): void => {
      if (debounceTimer !== null) {
        clearTimeout(debounceTimer);
      }
      debounceTimer = setTimeout(() => {
        debounceTimer = null;
        void commit(true, force);
      }, RESIZE_END_DEBOUNCE_MS);
    };

    const liveSync = (): void => {
      const now = Date.now();
      const elapsed = now - lastLiveSent;
      if (elapsed >= LIVE_SYNC_THROTTLE_MS) {
        lastLiveSent = now;
        void commit(false);
      } else if (liveTimer === null) {
        liveTimer = setTimeout(() => {
          liveTimer = null;
          lastLiveSent = Date.now();
          void commit(false);
        }, LIVE_SYNC_THROTTLE_MS - elapsed);
      }
    };

    const onGeometryChange = (): void => {
      liveSync();
      scheduleEndCommit();
    };

    void commit(true, true);
    const observer = new ResizeObserver(onGeometryChange);
    observer.observe(el);
    window.addEventListener("resize", onGeometryChange);
    const offLayout = onLayoutSettled((event) => scheduleEndCommit(event.force === true));

    return () => {
      cancelled = true;
      observer.disconnect();
      window.removeEventListener("resize", onGeometryChange);
      offLayout();
      if (debounceTimer !== null) {
        clearTimeout(debounceTimer);
      }
      if (liveTimer !== null) {
        clearTimeout(liveTimer);
      }
    };
  }, []);

  // RMB fly-cam: hold RMB over the viewport to fly. Pointer lock gives relative
  // mouse deltas (movementX/Y), which accumulate and stream with the WASD/Space/
  // Shift key state over `fly-input`. ESC exits pointer lock natively → fly ends.
  useEffect(() => {
    const el = hostRef.current;
    if (!el) {
      return;
    }

    const keys = { forward: false, back: false, left: false, right: false, up: false, down: false };
    let lookDx = 0;
    let lookDy = 0;
    let flying = false;
    let sendTimer: ReturnType<typeof setTimeout> | null = null;

    const sendState = (active: boolean): void => {
      const dx = lookDx;
      const dy = lookDy;
      lookDx = 0;
      lookDy = 0;
      void client.flyInput({ active, lookDx: dx, lookDy: dy, ...keys }).catch(() => {});
    };

    const scheduleSend = (): void => {
      if (sendTimer !== null) {
        return;
      }
      sendTimer = setTimeout(() => {
        sendTimer = null;
        if (flying) {
          sendState(true);
        }
      }, FLY_STREAM_MS);
    };

    const endFly = (): void => {
      if (!flying) {
        return;
      }
      flying = false;
      if (sendTimer !== null) {
        clearTimeout(sendTimer);
        sendTimer = null;
      }
      for (const key of Object.keys(keys) as (keyof typeof keys)[]) {
        keys[key] = false;
      }
      lookDx = 0;
      lookDy = 0;
      if (document.pointerLockElement === el) {
        document.exitPointerLock();
      }
      sendState(false);
    };

    const onPointerDown = (event: PointerEvent): void => {
      if (event.button !== 2 || flying) {
        return;
      }
      event.preventDefault();
      flying = true;
      el.requestPointerLock();
      sendState(true);
    };

    const onPointerUp = (event: PointerEvent): void => {
      if (event.button === 2) {
        endFly();
      }
    };

    const onPointerMove = (event: PointerEvent): void => {
      if (!flying) {
        return;
      }
      lookDx += event.movementX;
      lookDy += event.movementY;
      scheduleSend();
    };

    // Map a physical key code to a fly direction via the configured (hold-kind)
    // bindings. Read live from the store so a rebind in settings applies without
    // re-running this pointer-lock effect.
    const keyFor = (code: string): keyof typeof keys | null => {
      const overrides = useEditorStore.getState().keyBindings;
      if (code === bindingFor("camera.flyForward", overrides)) {
        return "forward";
      }
      if (code === bindingFor("camera.flyBack", overrides)) {
        return "back";
      }
      if (code === bindingFor("camera.flyLeft", overrides)) {
        return "left";
      }
      if (code === bindingFor("camera.flyRight", overrides)) {
        return "right";
      }
      if (code === bindingFor("camera.flyUp", overrides)) {
        return "up";
      }
      if (code === bindingFor("camera.flyDown", overrides)) {
        return "down";
      }
      return null;
    };

    const onKey =
      (down: boolean) =>
      (event: KeyboardEvent): void => {
        if (!flying) {
          return;
        }
        const key = keyFor(event.code);
        if (!key) {
          return;
        }
        event.preventDefault();
        if (keys[key] !== down) {
          keys[key] = down;
          scheduleSend();
        }
      };

    const onLockChange = (): void => {
      if (flying && document.pointerLockElement !== el) {
        endFly();
      }
    };

    const onContextMenu = (event: Event): void => event.preventDefault();

    const keyDown = onKey(true);
    const keyUp = onKey(false);
    el.addEventListener("pointerdown", onPointerDown);
    window.addEventListener("pointerup", onPointerUp);
    window.addEventListener("pointermove", onPointerMove);
    window.addEventListener("keydown", keyDown);
    window.addEventListener("keyup", keyUp);
    document.addEventListener("pointerlockchange", onLockChange);
    el.addEventListener("contextmenu", onContextMenu);

    return () => {
      endFly();
      el.removeEventListener("pointerdown", onPointerDown);
      window.removeEventListener("pointerup", onPointerUp);
      window.removeEventListener("pointermove", onPointerMove);
      window.removeEventListener("keydown", keyDown);
      window.removeEventListener("keyup", keyUp);
      document.removeEventListener("pointerlockchange", onLockChange);
      el.removeEventListener("contextmenu", onContextMenu);
    };
  }, []);

  // Pointer interaction: every press sends `begin`; if the pointer then travels
  // past DRAG_THRESHOLD_PX it is a gizmo drag (streamed `drag` + dragActive guard),
  // otherwise the release is a click that ray-picks. Release always sends `end`.
  // A bare move (no button down) streams `hover` so the engine highlights handles.
  useEffect(() => {
    const el = hostRef.current;
    if (!el) {
      return;
    }

    // Press-gesture state, reset on each pointerdown / pointerup.
    let pointerId: number | null = null;
    let startUv: Uv | null = null;
    let startClientX = 0;
    let startClientY = 0;
    let dragging = false;

    const ndc = (uv: Uv): { x: number; y: number } => ({
      x: uv.u * 2 - 1,
      y: uv.v * 2 - 1,
    });

    const onPointerDown = (event: PointerEvent): void => {
      // Left button only; RMB is the fly-cam (pointer lock) gesture.
      if (event.button !== 0 || pointerId !== null || document.pointerLockElement === el) {
        return;
      }
      pointerId = event.pointerId;
      startUv = eventToUv(el, event);
      startClientX = event.clientX;
      startClientY = event.clientY;
      dragging = false;
      el.setPointerCapture(event.pointerId);
      const { x, y } = ndc(startUv);
      void client.gizmoPointer("begin", x, y).catch(() => {});
    };

    const onPointerMove = (event: PointerEvent): void => {
      // While pointer lock is held the fly-cam owns the pointer; client coords are stale.
      if (document.pointerLockElement === el) {
        return;
      }
      const uv = eventToUv(el, event);
      if (pointerId === null) {
        // Hovering (no button down): keep the engine's handle highlight fresh.
        hoverCoalescer.push(uv);
        return;
      }
      if (event.pointerId !== pointerId) {
        return;
      }
      if (!dragging) {
        const moved =
          Math.abs(event.clientX - startClientX) > DRAG_THRESHOLD_PX ||
          Math.abs(event.clientY - startClientY) > DRAG_THRESHOLD_PX;
        if (!moved) {
          return;
        }
        dragging = true;
        setDragActive(true);
      }
      dragCoalescer.push(uv);
    };

    const finishPress = (event: PointerEvent): void => {
      if (pointerId === null || event.pointerId !== pointerId) {
        return;
      }
      const uv = eventToUv(el, event);
      const { x, y } = ndc(uv);
      void client.gizmoPointer("end", x, y).catch(() => {});
      if (el.hasPointerCapture(event.pointerId)) {
        el.releasePointerCapture(event.pointerId);
      }
      const wasDragging = dragging;
      const downUv = startUv;
      pointerId = null;
      startUv = null;
      dragging = false;
      if (wasDragging) {
        // The authoritative transform is committed engine-side on `end`; let the
        // poll resume and reconcile it.
        setDragActive(false);
      } else if (downUv) {
        // No drag: a plain left-click → ray-pick at the press location.
        void runPick(downUv);
      }
    };

    el.addEventListener("pointerdown", onPointerDown);
    el.addEventListener("pointermove", onPointerMove);
    el.addEventListener("pointerup", finishPress);
    el.addEventListener("pointercancel", finishPress);

    return () => {
      el.removeEventListener("pointerdown", onPointerDown);
      el.removeEventListener("pointermove", onPointerMove);
      el.removeEventListener("pointerup", finishPress);
      el.removeEventListener("pointercancel", finishPress);
      if (pointerId !== null && el.hasPointerCapture(pointerId)) {
        el.releasePointerCapture(pointerId);
      }
      // Drop any drag guard the gesture left set.
      if (dragging) {
        setDragActive(false);
      }
    };
  }, [hoverCoalescer, dragCoalescer, runPick, setDragActive]);

  // h-full/w-full (not flex-1): the panel's content div is block-level, so flex-1
  // would be inert here — the viewport must fill its panel rect by explicit size.
  // Transparent while live (the hole down to the subsurface); opaque while parked so
  // a modal over the region does not show the desktop through the window.
  return (
    <div
      className={`relative h-full w-full overflow-hidden ${
        viewportHidden ? "bg-background" : "bg-transparent"
      }`}
    >
      <div ref={hostRef} className="viewport-host" />
      <LoadingOverlay />
    </div>
  );
}
