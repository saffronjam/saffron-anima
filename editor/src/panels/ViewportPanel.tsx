/// The viewport region: a div the native SaffronEngine window is reparented OVER.
/// It never renders pixels — it only owns the screen rectangle and keeps the native
/// window glued to it. On mount it waits for a non-zero rect, then attaches; on any
/// later size/position change it resizes (diffed, debounced). A <LoadingOverlay/>
/// sibling covers the region while the renderer is not yet ready.
import { useCallback, useEffect, useLayoutEffect, useMemo, useRef } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { client, type ViewportBounds } from "../control/client";
import { makeCoalescer } from "../control/coalesce";
import { useEditorStore } from "../state/store";
import { LoadingOverlay } from "../app/LoadingOverlay";
import { onLayoutSettled } from "../app/layoutBus";

/// Resize-end commit debounce: after the last layout/resize change settles, send one
/// final exact bounds so the native window lands precisely even if the live throttle
/// dropped the last frame of a drag.
const RESIZE_END_DEBOUNCE_MS = 150;

/// Live-sync throttle during a drag: the native window tracks the host div at most
/// this often so a fast split-drag does not flood the control socket (it may lag
/// <=1 frame, accepted — same tradeoff as the C++ viewport resize).
const LIVE_SYNC_THROTTLE_MS = 50;

/// Pointer travel (CSS px) below which a press-release is treated as a click
/// (ray-pick) rather than a gizmo drag.
const DRAG_THRESHOLD_PX = 3;

/// Throttle for streamed gizmo pointer phases (hover/drag), in milliseconds.
const GIZMO_STREAM_MS = 16;

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

function boundsEqual(a: ViewportBounds | null, b: ViewportBounds): boolean {
  return (
    a !== null &&
    a.x === b.x &&
    a.y === b.y &&
    a.width === b.width &&
    a.height === b.height
  );
}

/// Read the div's CSS rect, scale to device pixels (HiDPI), and round.
async function computeBounds(el: HTMLElement): Promise<ViewportBounds | null> {
  const rect = el.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) {
    return null;
  }
  const scale = await getCurrentWindow().scaleFactor();
  return {
    x: Math.round(rect.x * scale),
    y: Math.round(rect.y * scale),
    width: Math.round(rect.width * scale),
    height: Math.round(rect.height * scale),
  };
}

/// Off-screen rect the native viewport is parked at while an overlay (the asset
/// View modal) needs to paint over the viewport region. The reparented X11 child
/// always paints on top of the webview, so we move it out of sight rather than try
/// to draw over it. 1×1 (not 0) keeps the swapchain valid.
const PARKED_BOUNDS: ViewportBounds = { x: -10000, y: -10000, width: 1, height: 1 };

export function ViewportPanel() {
  const hostRef = useRef<HTMLDivElement | null>(null);
  const lastBoundsRef = useRef<ViewportBounds | null>(null);
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

  // Attach: wait for a real (non-zero) rect, then reparent the native window over it.
  useLayoutEffect(() => {
    const el = hostRef.current;
    if (!el) {
      return;
    }
    let cancelled = false;
    let rafId = 0;

    const tryAttach = async (): Promise<void> => {
      if (cancelled) {
        return;
      }
      const bounds = await computeBounds(el);
      if (cancelled) {
        return;
      }
      if (!bounds) {
        // Layout not settled yet; retry next frame.
        rafId = requestAnimationFrame(() => void tryAttach());
        return;
      }
      if (attachedRef.current) {
        return;
      }
      // The `engine-phase` events are emitted from the Rust `.setup()` hook BEFORE this
      // webview registers its listener (Tauri does not buffer pre-listen events), so we
      // must NOT gate the attach on `phase`. Probe the control plane directly instead:
      // `viewport-native-info` starts succeeding once the engine has booted + bound its
      // socket. Retry on a timer until it does, then attach. (`cancelled` makes any
      // pending retry a no-op after unmount.)
      try {
        await client.raw("viewport-native-info");
      } catch {
        if (cancelled) {
          return;
        }
        setTimeout(() => void tryAttach(), 150);
        return;
      }
      if (cancelled) {
        return;
      }
      setPhase("attaching");
      try {
        await client.attachViewport(bounds);
        if (cancelled) {
          return;
        }
        attachedRef.current = true;
        lastBoundsRef.current = bounds;
        setPhase("ready");
        // Dev diagnostic: VITE_PARK_VIEWPORT keeps the engine running and feeding the UI
        // but holds its native window off-screen, so the webview chrome is never composited
        // over — a way to feel the populated editor without the reparented viewport on top.
        if (import.meta.env.VITE_PARK_VIEWPORT) {
          useEditorStore.getState().setViewportHidden(true);
        }
      } catch (err) {
        if (cancelled) {
          return;
        }
        setPhase("error", String(err));
      }
    };

    void tryAttach();

    return () => {
      cancelled = true;
      if (rafId) {
        cancelAnimationFrame(rafId);
      }
    };
  }, [setPhase]);

  // Bounds-sync: keep the native window glued on resize / dock split changes. Two
  // tiers (per the phase-9 spec):
  //   - a THROTTLED live sync (~50ms) on every host-div geometry change (the
  //     ResizeObserver fires during a drag) so the native window roughly tracks;
  //   - a single DEBOUNCED resize-end commit (~150ms) so the final bounds land
  //     exactly even if the throttle dropped the last drag frame.
  // The layout bus (Layout's onLayoutChanged) drives the same resize-end commit so
  // a panel-split that settles without further div mutation still commits. Both
  // paths share the diff guard, the scaleFactor() multiply (inside computeBounds),
  // and the viewportHidden park.
  useEffect(() => {
    const el = hostRef.current;
    if (!el) {
      return;
    }
    let cancelled = false;
    let debounceTimer: ReturnType<typeof setTimeout> | null = null;
    let lastLiveSent = 0;
    let liveTimer: ReturnType<typeof setTimeout> | null = null;

    const commit = async (): Promise<void> => {
      if (cancelled || !attachedRef.current) {
        return;
      }
      // While the viewport is parked off-screen (a modal is open), don't glue the
      // native window back to the div — the park/restore effect owns it.
      if (useEditorStore.getState().viewportHidden) {
        return;
      }
      const bounds = await computeBounds(el);
      if (cancelled || !bounds) {
        return;
      }
      if (boundsEqual(lastBoundsRef.current, bounds)) {
        return;
      }
      lastBoundsRef.current = bounds;
      try {
        await client.resizeViewport(bounds);
      } catch {
        // The window may be detached mid-resize; the next sync recovers.
      }
    };

    // Resize-end commit: a final exact bounds even if the throttle dropped the last
    // frame of a drag. Shared by the geometry observer and the layout bus.
    const scheduleEndCommit = (): void => {
      if (debounceTimer !== null) {
        clearTimeout(debounceTimer);
      }
      debounceTimer = setTimeout(() => {
        debounceTimer = null;
        void commit();
      }, RESIZE_END_DEBOUNCE_MS);
    };

    // Live sync, throttled: commit at most once per LIVE_SYNC_THROTTLE_MS while the
    // div geometry keeps changing (a drag), with a trailing call so the throttle
    // window never swallows the last change before the end commit.
    const liveSync = (): void => {
      const now = Date.now();
      const elapsed = now - lastLiveSent;
      if (elapsed >= LIVE_SYNC_THROTTLE_MS) {
        lastLiveSent = now;
        void commit();
      } else if (liveTimer === null) {
        liveTimer = setTimeout(() => {
          liveTimer = null;
          lastLiveSent = Date.now();
          void commit();
        }, LIVE_SYNC_THROTTLE_MS - elapsed);
      }
    };

    const onGeometryChange = (): void => {
      liveSync();
      scheduleEndCommit();
    };

    const observer = new ResizeObserver(onGeometryChange);
    observer.observe(el);
    window.addEventListener("resize", onGeometryChange);
    // A panel-split drag-end fires onLayoutChanged; commit the exact bounds once.
    const offLayout = onLayoutSettled(scheduleEndCommit);

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

  // Occlusion handling: when `viewportHidden` is set (the asset View modal is
  // open) park the native window off-screen so the modal — a normal webview DOM
  // overlay — is visible over the viewport rect; restore the native window to the
  // div's current bounds when it clears. lastBoundsRef is left untouched so the
  // bounds-sync diff re-glues correctly afterwards.
  useEffect(() => {
    const el = hostRef.current;
    if (!el || !attachedRef.current) {
      return;
    }
    let cancelled = false;
    const apply = async (): Promise<void> => {
      try {
        if (viewportHidden) {
          await client.resizeViewport(PARKED_BOUNDS);
          return;
        }
        const bounds = await computeBounds(el);
        if (cancelled || !bounds) {
          return;
        }
        lastBoundsRef.current = bounds;
        await client.resizeViewport(bounds);
      } catch {
        // The window may be mid-detach; the bounds-sync poll recovers on restore.
      }
    };
    void apply();
    return () => {
      cancelled = true;
    };
  }, [viewportHidden]);

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
      // Left button only; the engine owns RMB-look / camera input.
      if (event.button !== 0 || pointerId !== null) {
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
  return (
    <div className="relative h-full w-full overflow-hidden bg-black">
      <div ref={hostRef} className="viewport-host" />
      <LoadingOverlay />
    </div>
  );
}
