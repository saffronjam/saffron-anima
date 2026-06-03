# In-webview viewport presentation

Stop presenting the engine viewport as a **native X11 child window reparented over the
webview**, and instead give the webview an unobstructed surface that shows the engine's
rendered frames. This removes the single thing that drags the whole editor UI below its
refresh rate.

**Status:** NOT STARTED

## Why (measured, not theorised)

The editor chrome (menus, panels, the Create dropdown) felt sluggish — ~50–60 fps on
144/240 Hz displays, where a browser is smooth. A debug session ruled out the obvious
suspects one by one:

- CSS/animation cost — not it (all menu animations are already transform/opacity).
- The GTK GSK renderer (`GSK_RENDERER=ngl|cairo`) and `WEBKIT_DISABLE_DMABUF_RENDERER` —
  **no difference**.
- XWayland vs native Wayland — bare WebKitGTK 2.52 (MiniBrowser/epiphany) reads **62 fps on
  both backends**, so that ~60 is WebKit's own animation clock, a minor ceiling.
- The decisive A/B: running the **real** editor with the engine attached vs. with the
  engine window **parked off-screen** (`VITE_PARK_VIEWPORT`, `ViewportPanel.tsx`). Same
  engine, same GPU, same scene — only the overlap differs. **Parked is dramatically
  smoother.**

Root cause: WebKitGTK paints the entire React UI through one GPU-composited GL surface (its
fast path). The viewport is a foreign X11 child window reparented into that GTK window
(`control_commands_render.cpp:385` `XReparentWindow`; editor `AGENTS.md`: *"an X11 child
painted on top of the webview"*). A native X11 subwindow overlapping the webview forces
GTK3+X off the single-surface path: the **X server** now stacks and clips two separate
native windows every frame, throttling frame delivery for the *whole* webview — not just the
viewport rect. The editor already hits this and works around it by parking the viewport
whenever a modal must paint over it (`ViewportPanel.tsx:76` `PARKED_BOUNDS`).

This plan removes the overlapping native window entirely. It closes Open Question #4 of
[`../typescript-ui-migration/README.md`](../typescript-ui-migration/README.md) (the
deferred fd-export/DMA-BUF path) with a measured justification.

## What already exists (the work is mostly plumbing, not new rendering)

- The engine in present-only mode **already renders the viewport to an offscreen image**
  (`renderer.targets.offscreen`, `OffscreenColorFormat`), then only blits it to the
  swapchain at the very end (`presentViewportToSwapchain`, `renderer.cppm:1565`). The
  swapchain/native window is the *last* hop, and the only hop we are replacing.
- The engine **already reads that image back** to a host-mapped buffer for screenshots
  (`captureViewport`, `renderer_capture.cpp:38`; `newHostCaptureBuffer`,
  `renderer_detail.cppm:923`). Per-frame readback is the same operation made async.
- The webview **already displays engine-produced images** — asset thumbnails travel as
  PNG bytes and become a `<img>`/blob URL (`store.ts:406` `base64ToBlob`, `:419`
  `getThumbnailUrl`). The viewport surface is the same idea at video rate.
- Input is **already transport-only**: pick/gizmo/hover go over the control socket as
  `u,v`/NDC (`ViewportPanel.tsx:38` `eventToUv`; `client.ts:145` `pick`, `:156`
  `gizmoPointer`), independent of how pixels are shown. Nothing about input depends on the
  native window.

## Architecture decision

Two ways to show frames without an overlapping native window:

1. **Frame-streaming (portable).** Engine renders offscreen → async GPU→CPU readback to a
   shared buffer → webview uploads it to a `<canvas>`/WebGL texture each frame. Works on any
   webview/compositor, fully eliminates the native window. Cost: a per-frame readback +
   upload + one frame of latency — the very copy the native-child design was built to avoid.
2. **Zero-copy compositor overlay (Wayland subsurface / dmabuf).** Engine renders into a
   dmabuf-backed image (`VK_EXT_external_memory_dma_buf`) composited as a `wl_subsurface`
   under the webview surface (native Wayland). A subsurface is the Wayland-native way to
   stack a separate buffer **without** forcing the parent webview off its render path — so
   it keeps the chrome smooth *and* stays zero-copy. Higher risk (wry/gdk-wayland surface
   access, input/clip plumbing); a 417-line GTK4 fd-export prototype exists in git history
   (`viewport_bridge.rs`, referenced in the migration plan) as prior art.

**Sequence:** ship **frame-streaming first** (phases 1–3) — it is the guaranteed correctness
+ smoothness win and unblocks dropping the X11 force entirely. Then **measure** whether the
added viewport latency is acceptable for camera-orbit/gizmo-drag; if not, restore zero-copy
via the subsurface path (phase 4). Phases 1–3 are the committed fix; phase 4 is a
measurement-gated optimization, not a prerequisite.

## Phases (dependency order)

| # | Phase | File | Depends on |
|---|-------|------|-----------|
| 1 | Windowless offscreen viewport + async readback ring (engine) | [`phase-1-windowless-offscreen-viewport.md`](phase-1-windowless-offscreen-viewport.md) | — |
| 2 | Frame transport (shared buffer + custom protocol) + in-webview canvas surface | [`phase-2-frame-transport-and-canvas-surface.md`](phase-2-frame-transport-and-canvas-surface.md) | phase 1 |
| 3 | Input/coords re-point, lifecycle without attach, drop the X11/SDL-x11 force | [`phase-3-input-lifecycle-drop-x11.md`](phase-3-input-lifecycle-drop-x11.md) | phase 2 |
| 4 | Zero-copy restore: dmabuf image + Wayland subsurface (measurement-gated) | [`phase-4-zero-copy-dmabuf-wayland-subsurface.md`](phase-4-zero-copy-dmabuf-wayland-subsurface.md) | phase 3 |

## Keep-current obligations

- **`se` CLI / control:** `viewport-native-info`'s `transport` field changes from
  `"x11-child-window"`; `attach-native-viewport` / `resize-native-viewport` are removed or
  repurposed to size-only (phase 3). Any new state (frame size, transport handle) gets a
  control command so the running editor stays scriptable.
- **`docs/`:** the editor/viewport explanation under `docs/content/` must be updated in the
  phase that lands the user-visible change (phase 3) to describe the in-webview surface
  instead of the X11 reparent.

## Non-goals / out of scope

- **Multi-viewport.** One viewport surface, as today.
- **Phase 4 is not required to call this done.** If streaming latency is fine, phases 1–3
  are the whole fix and phase 4 is deleted unstarted.
- **Windows/macOS transport specifics.** The native-child problem is the Linux/WebKitGTK
  path; this plan targets it. The frame-streaming surface is portable, but other platforms
  are not validated here.

## Open questions

1. **Readback latency (gates phase 4):** is one frame of GPU→CPU→upload latency acceptable
   for camera-orbit and gizmo-drag at 1080p/1440p viewport sizes on the 3070 Ti? Phase 2
   instruments this (readback ms, end-to-end latency); the number decides whether phase 4
   runs.
2. **Transport for the frame bytes (phase 2):** shared-memory file (`memfd`/shm in
   `XDG_RUNTIME_DIR`) mmap'd by both engine and the Rust shell, surfaced to the webview via a
   Tauri custom URI scheme returning raw bytes — vs. a second binary unix socket. Default:
   shm + custom protocol + a seqno header the webview polls; avoids base64 and JSON for the
   hot payload.
3. **Color/transfer on the wire (phase 1/2):** the offscreen is post-tonemap display-referred
   `R16F`; readback resolves to `RGBA8` sRGB (one blit, same conversion the swapchain blit
   already does). Confirm premultiplied-alpha + sRGB handling matches `<canvas>` expectations.
4. **Subsurface viability (phase 4 spike):** can wry/gdk expose the webview's `wl_surface`
   for a child subsurface, with correct stacking/clipping under the viewport rect and pointer
   routing? The old `viewport_bridge.rs` is a starting reference, not a drop-in.
