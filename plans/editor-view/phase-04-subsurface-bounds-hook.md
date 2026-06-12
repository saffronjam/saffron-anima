# Phase 4 — the subsurface bounds hook

**Status:** COMPLETED (kept as a build record; superseded by the general Asset editor — see this dir's README)
**Depends on:** — (a self-contained frontend refactor)

## Goal

A pure editor refactor with **no behavior change**: extract the viewport's bounds-emission machinery
out of `ViewportPanel` into a reusable `useSubsurfaceBounds(hostRef)` hook, so phase 5 can glue the
engine's Wayland subsurface into the rig editor's preview pane. The transport sink is a singleton but
the emitter is not — any component may drive `set_viewport_bounds`, and the parked dock's degenerate
rect no-ops — so this is the entire frontend transport cost of the live preview.

## What exists to build on

- `ViewportPanel` owns the host div + the emission chain (`editor/src/panels/ViewportPanel.tsx`):
  `computeBounds(el)` reads the rect + scale factor and returns `null` for degenerate (≤0) rects
  (`:38-51` — the guard that makes a hidden host inert); the bounds-sync effect (`:199-280`)
  combines a ResizeObserver, `window.resize`, and the layout bus into a 16 ms-throttled live sync
  plus a 150 ms-debounced end commit (`commit(true)` ⇒ `resizeEngine`); the send is
  `client.setViewportBounds(bounds, resizeEngine)` (`:226`).
- The Rust side is component-agnostic: `set_viewport_bounds` (`lib.rs:411-444`) writes the
  `ViewportShared` atomics and, on `resize_engine`, sends `set-viewport-size`;
  `set_viewport_hidden` parks (`:446-461`); one subsurface, one presenter
  (`wayland_viewport.rs:51-68`, `:474-527`).
- The parking interplay: while a non-scene tab is active the dock is `display:none`
  (`App.tsx:219`), the host rect is 0×0, and commits no-op via the null guard — documented in the
  JSX comment at `App.tsx:214-218`.
- The layout bus: `emitLayoutSettled` (`layoutBus.ts:27-31`); `App.tsx:193` force-emits when the
  scene tab re-activates.

## Work

### 1. Extract `useSubsurfaceBounds(hostRef, opts?)`

Move `computeBounds` + the bounds-sync effect into `editor/src/lib/useSubsurfaceBounds.ts` (or
`panels/` if convention prefers): same ResizeObserver + resize + layout-bus wiring, same two-tier
throttle/debounce constants (`ViewportPanel.tsx:18`, `:23`), same `client.setViewportBounds` calls.
`ViewportPanel` consumes the hook and keeps everything else (input, overlays, drag handling)
unchanged.

### 2. Keep single-emitter semantics by construction

Two hosts must never both be non-degenerate: the dock host is 0×0 whenever a non-scene tab is
active (existing), and the rig workspace only mounts while its tab is active (App.tsx conditional
mounts unmount inactive workspaces). Document this invariant in the hook's header comment — it is
the reason no arbitration/locking is needed.

## Validation (done criteria)

- `bun run check` + `bun run lint` clean; no new warnings.
- **No behavior change**: `make run` — viewport positions/resizes exactly as before across dock
  splits, window resize, tab switches (scene → flame graph → scene re-glues via the existing
  force-emit), project load. The bounds-sync constants and order of commits are unchanged (diff
  review, not just eyeballing).
- `make e2e` unaffected (the suite drives headless engines, not the Tauri shell — this is a
  frontend-only phase; the manual check above is the real gate).

## Notes / gotchas

- This is the highest-risk *kind* of change in the editor (the subsurface desync failure mode is
  visible and ugly), in the lowest-risk *form* — a move, not a rewrite. Resist improving the logic
  while moving it; any throttle/debounce tuning is a separate change.
- `AssetPreview` (`AssetViewer.tsx:63-68`) also writes `viewportHidden` on mount/unmount — that
  redundancy is unrelated to this hook; leave it.
- The stale "reparented X11 child" comment in `AssetViewer.tsx:3-6` can be corrected if touched,
  but do not expand scope otherwise.
