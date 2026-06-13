/// The Wayland subsurface bounds emitter, extracted from ViewportPanel so a second host rect (the
/// asset editor's preview pane) can drive the same single subsurface. The transport SINK is a singleton
/// (one subsurface, one presenter in wayland_viewport.rs), but the EMITTER is not: any component may
/// call set_viewport_bounds, and a degenerate (<=0) rect no-ops via the computeBounds null guard.
///
/// INVARIANT — at most one host drives the subsurface at a time, so no arbitration/locking is needed:
/// the dock viewport host is 0x0 whenever a non-scene tab is active (App.tsx parks it via viewportHidden
/// + the panel collapses), and the asset workspace — though kept mounted across tab switches — passes
/// `enabled: false` whenever its tab isn't active. Two emitters therefore never both drive the subsurface.
import { useEffect, type RefObject } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { client, type ViewportBounds } from "../control/client";
import { onLayoutSettled } from "../app/layoutBus";
import { waitForFreshFrame } from "./waitForFreshFrame";

/// Resize-end commit debounce: after the last layout/resize change settles, send one final exact
/// bounds — this is the tier that also resizes the engine's render target (expensive offscreen
/// recreation, so never on live drag ticks).
const RESIZE_END_DEBOUNCE_MS = 150;

/// Live-sync throttle during a drag: the subsurface geometry (position + stretched destination)
/// tracks the host div at this cadence; the engine keeps rendering at the old size until the end commit.
const LIVE_SYNC_THROTTLE_MS = 16;

/// Read the div's logical CSS rect + the window scale factor. Rust positions the subsurface in logical
/// units and derives the engine's device-pixel render size. A degenerate (<=0) rect returns null, which
/// makes a hidden/parked host inert.
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

/// Glue the engine's subsurface to the given host div: keep it positioned on resize / dock-split /
/// layout changes. Two tiers:
///   - a THROTTLED live sync (~16ms) on every host-div geometry change (the ResizeObserver fires
///     during a drag) so the subsurface roughly tracks;
///   - a single DEBOUNCED resize-end commit (~150ms) so the final bounds land exactly even if the
///     throttle dropped the last drag frame — this tier also resizes the engine's render target.
/// The layout bus drives the same resize-end commit so a panel split that settles without further div
/// mutation still commits.
export function useSubsurfaceBounds(
  hostRef: RefObject<HTMLElement | null>,
  opts: { enabled?: boolean; onSettled?: () => void } = {},
): void {
  const { enabled, onSettled } = opts;
  useEffect(() => {
    const el = hostRef.current;
    if (!el || enabled === false) {
      return;
    }
    let cancelled = false;
    let settledFired = false;
    let lastSent: ViewportBounds | null = null;
    let debounceTimer: ReturnType<typeof setTimeout> | null = null;
    let lastLiveSent = 0;
    let liveTimer: ReturnType<typeof setTimeout> | null = null;

    // Live tier moves/stretches the subsurface only; the end tier (resizeEngine) also commits the
    // device-pixel render size so the engine re-renders sharp at the final rect — once per gesture
    // instead of per drag tick.
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
        debounceTimer = null;
      }
      // A forced settle (tab-show / layout-settled) resizes immediately so the reveal isn't delayed by
      // the drag-coalescing debounce — the consumer's mask then lifts on the next fresh frame. Live drag
      // ticks still debounce via the non-forced path.
      if (force) {
        void commit(true, true);
        return;
      }
      debounceTimer = setTimeout(() => {
        debounceTimer = null;
        void commit(true, false);
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

    // The first forced commit resizes the engine's render target at the host's settled size; reveal the
    // viewport only once the presenter has actually DISPLAYED a fresh frame at the new size (not after a
    // timer guess), so the resize never flashes the stretched old frame through the mask.
    void commit(true, true).then(async () => {
      if (cancelled || settledFired || !onSettled) {
        return;
      }
      settledFired = true;
      await waitForFreshFrame();
      if (!cancelled) {
        onSettled();
      }
    });
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
  }, [hostRef, enabled, onSettled]);
}
