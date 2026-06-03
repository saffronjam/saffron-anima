# Phase 3 — Input/coords re-point, lifecycle without attach, drop the X11 force

**Status:** NOT STARTED

**Depends on:** phase 2

## Goal

Finish the cutover: point input at the canvas, replace the attach/reparent lifecycle with a
first-frame-ready lifecycle, delete the now-dead native-window machinery, and **remove the
`GDK_BACKEND=x11` / `SDL_VIDEODRIVER=x11` force** so the editor runs native Wayland — which
also lifts the webview off XWayland. After this phase the native child window is gone.

## Steps

### Input (small — coordinates already exist)

1. **Re-point pointer mapping to the canvas.** `eventToUv` (`ViewportPanel.tsx:38`) and the
   NDC conversion (`:354`) read the host element's client rect — they work unchanged against
   the `<canvas>`. Confirm pick (`client.pick`, `client.ts:145`) and gizmo
   (`client.gizmoPointer`, `:156`) still round-trip; nothing about hit-testing depended on
   the native window. Hover/drag coalescers (`GIZMO_STREAM_MS`) are untouched.

### Lifecycle (replace attach with first-frame)

2. **Drop the attach handshake.** Remove the `attach_native_viewport` Tauri command +
   `parent_xid` (`lib.rs:236`,`:113`) and the engine `attach-native-viewport` /
   `XReparentWindow` path (`control_commands_render.cpp:323`). `resize-native-viewport`
   becomes **size-only** (it already does `XMoveResize` + `setViewportDesiredSize`,
   `:407`) → keep only the `setViewportDesiredSize` half, rename to a viewport-size command.

3. **`phase === 'ready'` on first frame.** The attach success that set `ready`
   (`ViewportPanel.tsx:180`) is gone; instead flip to `ready` when the first frame with a
   valid `seqno` arrives (phase 2). The readiness probe (`viewport-native-info`,
   `:161`) stays as the "engine socket is up" gate. Crash/restart watchdog
   (reconcile poll) is unchanged.

4. **Delete the occlusion hacks.** `PARKED_BOUNDS` / `viewportHidden` and the park/restore
   effect (`ViewportPanel.tsx:76`,`:303`) exist only because the X11 child painted on top.
   A `<canvas>` is a normal DOM element — modals, menus, and the asset View overlay just
   stack via `z-index`. Remove the park logic and the `VITE_PARK_VIEWPORT` dev gate (added
   during the debug session) — it is obsoleted by this phase.

### Drop X11 (the secondary win)

5. **Remove the backend force.** Delete the `GDK_BACKEND=x11` set (`lib.rs:343`) and
   `SDL_VIDEODRIVER=x11` (`lib.rs:175`) now that nothing reparents an XID. The engine renders
   windowless (phase 1) so SDL needs no presentable surface; the editor's GTK window picks
   native Wayland. Verify the bare-webview cap question: the chrome now runs on the native
   Wayland path (still subject to WebKit's ~62 Hz animation clock, but no XWayland layer).

6. **Update the contract + docs.** `viewport-native-info` `transport` reflects the new
   surface (not `"x11-child-window"`). Regenerate `@saffron/protocol` if any schema changes.
   Update the editor/viewport explanation under `docs/content/` (keep-current obligation) to
   describe the in-webview canvas surface and remove the reparenting description from the
   editor `AGENTS.md` rules.

## Validation

- Full editor parity with the native-child build: pick, gizmo T/R/S, hover highlight,
  resize on dock split, HiDPI, fly-cam — all work against the canvas.
- `make run` no longer sets `GDK_BACKEND`/`SDL_VIDEODRIVER` to x11; `xlsclients` / window
  inspection shows the editor as a Wayland client with **no child X11 window**.
- The footer `UI fps` holds at display refresh while the viewport renders and while opening
  menus — the original symptom is gone.
- `make check` / `make e2e` green; control schema contract test passes.

## Risks

- **Input focus / keyboard.** The native child previously could receive SDL keyboard for the
  fly-cam. With no native window, all camera/gizmo keys must come over control commands
  (`set-camera` / `set-gizmo`) — confirm the existing command path covers what was native
  (the migration plan already defaulted to command-driven input).
- **Removing `resize-native-viewport` shape** may ripple into the `se` CLI / e2e tests that
  call it — update those in the same change.
