+++
title = 'Viewport panel'
weight = 2
+++

# Viewport panel

The viewport panel renders no pixels. It is a `div` the engine's native SaffronEngine window is reparented *over* — the panel only owns the screen rectangle and keeps the native window glued to it. The 3D scene you see inside it is the engine presenting directly to its swapchain ([present-only mode](../tauri-editor-and-x11-bridge/)); the panel's job is bounds-sync, pointer forwarding, and occlusion handling.

## Attach to a real rect

On mount the panel waits for a non-zero layout rect, then attaches the native window over it. It also waits for the engine to be past the `starting` phase, because an attach before the engine's window exists would fail:

```ts
const bounds = await computeBounds(el);   // CSS rect × scaleFactor, rounded
if (!bounds) { rafId = requestAnimationFrame(tryAttach); return; }  // layout not settled
const phase = store.engineStatus.phase;
if (phase === "idle" || phase === "starting") { retry(); return; }
await client.attachViewport(bounds);
setPhase("ready");
```

`computeBounds` reads the div's CSS rect and multiplies by `window.scaleFactor()` so the native window is positioned in **physical pixels** on a HiDPI display, then rounds. The first successful attach is what flips the phase to `ready` and dismisses the [loading overlay](../tauri-editor-and-x11-bridge/), which lives as a sibling inside this panel.

## Bounds-sync

Once attached, the native window has to track the div on every dock split-resize, window resize, or panel rearrange. Two tiers keep it glued without flooding the socket:

- a **throttled live sync** (~50ms) on every geometry change — a `ResizeObserver` on the host div fires during a drag, so the native window roughly follows;
- a **debounced resize-end commit** (~150ms) that sends one final exact bounds so the window lands precisely even if the throttle dropped the last frame.

```ts
const observer = new ResizeObserver(onGeometryChange);  // live sync + schedule end-commit
observer.observe(el);
window.addEventListener("resize", onGeometryChange);
const offLayout = onLayoutSettled(scheduleEndCommit);   // a settled panel-split commits too
```

Both paths share a diff guard (skip if the bounds are unchanged), the `scaleFactor()` multiply, and the off-screen park. The resize uses the dedicated `resize-native-viewport` command (a move/resize only, no reparent), so a per-tick bounds update never re-`XReparent`s and flickers.

## The Radix-portal occlusion rule

The reparented X11 child always paints on top of its rect once mapped — the webview cannot draw over it. Any element that would overlap the viewport must therefore render elsewhere or while the native window is unmapped:

- the loading overlay is an inline sibling that only matters before the first attach (native window not yet mapped);
- the asset **View modal** sets `store.viewportHidden`; the panel reads it and parks the native window off-screen (a 1×1 rect far off the canvas) so the modal — a normal webview DOM overlay — shows over the viewport region, then restores it on close;
- every menu, dropdown, and asset/inspector popover is kept in a side dock so its portal never lands over the viewport rect.

This is the single rule that shapes where editor chrome can live; the [native bridge](../tauri-editor-and-x11-bridge/) page states it once for the whole editor.

## Pointer forwarding

The panel turns DOM pointer events into engine intent (the native child gets no raw mouse from the webview). A press sends [`gizmo-pointer begin`](../gizmo/); travel past a few pixels makes it a `drag` (streamed, with `dragActive` set so the poll backs off); the release sends `end`. A press that didn't travel is a click — it [ray-picks](../selection/) at the press UV. A bare move (no button) streams `hover` so the engine highlights the handle under the cursor. The left button only — RMB-look and WASD belong to the engine's [editor camera](../editor-camera/).

## In the code

| What | File | Symbols |
|---|---|---|
| The panel | `editor/src/panels/ViewportPanel.tsx` | `ViewportPanel`, `computeBounds`, `eventToUv` |
| Attach on mount | `editor/src/panels/ViewportPanel.tsx` | the attach `useLayoutEffect`, `attachViewport` |
| Two-tier bounds-sync | `editor/src/panels/ViewportPanel.tsx` | `liveSync`, `scheduleEndCommit`, `onLayoutSettled` |
| Off-screen park | `editor/src/panels/ViewportPanel.tsx` | `PARKED_BOUNDS`, `viewportHidden` |
| Reparent / resize (engine) | `control_commands_render.cpp` | `attach-native-viewport`, `resize-native-viewport`, `viewport-native-info` |

## Related

- [Tauri editor and the X11 bridge](../tauri-editor-and-x11-bridge/) — the present-only reparent this panel hosts
- [Gizmo](../gizmo/) — the pointer phases this panel forwards
- [Selection](../selection/) — click-pick from a non-drag press
- [Editor camera](../editor-camera/) — the RMB-look / WASD input the engine owns
