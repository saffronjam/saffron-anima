# Phase 2 — Frame transport + in-webview canvas surface

**Status:** NOT STARTED

**Depends on:** phase 1

## Goal

Carry the latest viewport frame from the engine to the webview with minimal copies, and draw
it into a `<canvas>` that replaces the reparented `viewport-host` div. After this phase the
viewport is a normal DOM element fed by the engine; no native window overlaps the webview.

## Transport (Open Question #2)

A shared-memory frame buffer, surfaced to the webview through a Tauri custom URI scheme — the
big payload never touches JSON or base64, and the control socket carries only tiny messages.

1. **Engine → shared memory.** Phase 1's readback ring writes into an `shm`/`memfd` file in
   `XDG_RUNTIME_DIR` (named per the control socket's pid, mirroring
   `saffron-editor-{pid}.sock`, `lib.rs:60`) with a small header:
   `{ seqno, width, height, format, stride }` followed by the pixel plane. Double/triple the
   plane so a torn read is impossible — writer advances `seqno` only after the plane is fully
   written (seqlock discipline).

2. **Rust shell maps the same file.** The editor backend (`lib.rs`) mmaps the shm region
   read-only. A new `viewport-frame-info` control command (engine side) returns the shm name
   + geometry so Rust can open it; the engine advertises this transport in
   `viewport-native-info` (`control_commands_render.cpp:308`) instead of
   `"x11-child-window"`.

3. **Webview pulls frames via a custom protocol.** Register a Tauri URI scheme
   (`register_uri_scheme_protocol`, e.g. `viewport://frame`) that returns the latest plane as
   raw bytes (an `ArrayBuffer`), with the `seqno` in a response header. This is the same
   "engine image → webview" shape as thumbnails (`store.ts:419`), but raw + per-frame instead
   of base64 + on-demand.

## Webview surface

4. **Replace the host div with a canvas.** In `ViewportPanel.tsx` swap the
   `<div ref={hostRef} className="viewport-host" />` (`ViewportPanel.tsx:440`) for a
   `<canvas>`. A WebGL context uploads the frame as a texture and draws a fullscreen quad
   (`texImage2D` / `texSubImage2D`); a 2D context (`putImageData`) is the simpler fallback if
   WebGL upload cost dominates. Keep the `LoadingOverlay` sibling.

5. **Drive at rAF, deduped by seqno.** A `requestAnimationFrame` loop fetches
   `viewport://frame`, and if its `seqno` advanced, uploads + draws; otherwise it skips the
   upload (no new engine frame). This naturally caps the webview's viewport draw at its own
   refresh and avoids re-uploading stale frames.

6. **Size negotiation, not window geometry.** The canvas's device-pixel size (client rect ×
   `scaleFactor()`, already computed in `computeBounds`, `ViewportPanel.tsx:62`) is sent as
   the desired viewport size via a size-only command (phase 3 finalizes the command). No
   `x,y` position is sent — there is no window to move.

## Validation

- Live scene renders in the canvas; orbiting the camera (control-command fly-cam) updates it.
- The chrome stays smooth while the viewport animates — confirm with the footer `UI fps`
  (`App.tsx:169`): it should sit at the display refresh, not collapse the way the X11-child
  path did. This is the headline result.
- Instrument and log: readback ms (phase 1) + transport fetch ms + texture-upload ms +
  end-to-end seqno→draw latency. These numbers decide Open Question #1 / whether phase 4 runs.
- No torn frames under a fast camera move (seqlock holds).

## Risks

- **Upload cost / bandwidth.** 1440p RGBA8 ≈ 14 MB/frame; at 60 fps that is ~850 MB/s of
  upload. Mitigations to keep in reserve: damage rects, half-res during active interaction,
  or a lightweight GPU-side encode. Measure first (step in Validation) before optimizing.
- **Custom-protocol per-frame overhead** in WebKitGTK. If `fetch` per frame is too costly,
  fall back to a `SharedArrayBuffer` fed from a Tauri event, or chunked streaming. Keep the
  seqno contract stable so the surface code is transport-agnostic.
