+++
title = 'Viewport panel'
weight = 2
+++

# Viewport panel

The viewport panel is the editor's window onto the 3D scene: a transparent `div` that owns
a screen rectangle and keeps the engine's subsurface glued to it. The panel renders no
pixels of its own. The scene inside it is the engine's render showing through the
transparent page ([viewport compositing](../viewport-compositing/)).

Its responsibilities are bounds-sync, input forwarding, and parking. The subsurface sits
below the webview, so the panel's role is to report where it should be, translate input
into engine intent, and hand the region back to the DOM when something else needs it.

## Bounds-sync

The panel reports its logical CSS rect plus the window scale factor through one Tauri
command, `set_viewport_bounds`. Rust fans it out: the logical rect (plus the webview's
CSD-aware offset within the toplevel, tracked GTK-side) positions and sizes the subsurface,
and the device-pixel size goes to the engine as `set-viewport-size` so the render matches
the panel one-to-one.

Two tiers keep the subsurface glued through dock drags without flooding the bridge:

- a **throttled live sync** (~50ms) on every geometry change — a `ResizeObserver` on the
  host div fires during a drag, so the subsurface roughly follows;
- a **debounced resize-end commit** (~150ms) that sends one final exact bounds, so it
  lands precisely even if the throttle dropped the last frame.

```ts
const observer = new ResizeObserver(onGeometryChange);  // live sync + schedule end-commit
observer.observe(el);
window.addEventListener("resize", onGeometryChange);
const offLayout = onLayoutSettled(scheduleEndCommit);   // a settled panel-split commits too
```

Both paths share a diff guard (skip if the bounds are unchanged). On mount the panel also
probes `viewport-native-info` until the engine's socket answers, then flips the phase to
`ready`, dismissing the loading overlay that covers the region until the first frame.

## Parking

Web UI composites freely over the live viewport — that is the point of the architecture —
but when the region should show no scene at all (the asset View modal, the asset
workspace tab), the store's `viewportHidden` flag parks the presenter: the subsurface
detaches its buffer and vanishes, and the panel paints an opaque background so the
transparent hole does not expose the desktop behind the window. `App` forwards the flag
over `set_viewport_hidden` so it works even while this panel is unmounted.

## Pointer forwarding

The panel turns DOM pointer events into engine intent — the engine's hidden window
receives no input at all. A left press sends [`gizmo-pointer begin`](../gizmo/); travel
past a few pixels makes it a `drag` (streamed, with `dragActive` set so the poll backs
off); the release sends `end`. A press that did not travel is a click — it
[ray-picks](../selection/) at the press UV. A bare move with no button streams `hover`,
so the engine highlights the handle under the cursor.

Holding the **right button** flies the [editor camera](../editor-camera/): the panel takes
pointer lock, accumulates relative deltas (`movementX/Y`) and the WASD/Space/Shift key
state, and streams them over `fly-input` (~16ms cadence; deltas accumulate between sends,
so nothing is lost). Releasing the button or pressing Escape (which exits pointer lock
natively) ends the fly.

## In the code

| What | File | Symbols |
|---|---|---|
| The panel | `editor/src/panels/ViewportPanel.tsx` | `ViewportPanel`, `computeBounds`, `eventToUv` |
| Two-tier bounds-sync | `editor/src/panels/ViewportPanel.tsx` | `liveSync`, `scheduleEndCommit`, `onLayoutSettled` |
| Pointer-lock fly streaming | `editor/src/panels/ViewportPanel.tsx` | the fly `useEffect`, `FLY_STREAM_MS` |
| Rect + park bridge (Rust) | `editor/src-tauri/src/lib.rs` | `set_viewport_bounds`, `set_viewport_hidden` |
| Subsurface side | `editor/src-tauri/src/wayland_viewport.rs` | `ViewportShared`, `install` |
| Render size (engine) | `control_commands_render.cpp` | `set-viewport-size`, `viewport-native-info` |

## Related

- [Viewport compositing](../viewport-compositing/) — the transport this panel positions
- [Tauri editor and the viewport bridge](../tauri-editor-and-viewport-bridge/) — the shell and lifecycle around it
- [Gizmo](../gizmo/) — the pointer phases this panel forwards
- [Selection](../selection/) — click-pick from a non-drag press
- [Editor camera](../editor-camera/) — the fly input this panel streams
