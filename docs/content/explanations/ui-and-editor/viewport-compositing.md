+++
title = 'Viewport compositing'
weight = 2
+++

# Viewport compositing

The editor's 3D viewport is the engine's render composited *under* the web UI: the engine
publishes each frame into shared memory, and the editor presents those frames on a Wayland
subsurface stacked below its own transparent window. Panels, shadows, rounded corners, and
translucent overlays therefore blend over the live scene — something a native child window
can never do, because window systems stack children opaquely on top ("airspace").

## How it works

```mermaid
flowchart LR
    A[engine renders offscreen] --> B[blit to BGRA8 + copy to staging<br>recorded in the frame's own command buffer]
    B --> C[shm ring segment<br>header + 4 slots, seqlock]
    C --> D[editor worker attaches the newest slot<br>to a wl_subsurface below the toplevel]
    D --> E[compositor blends the transparent UI<br>over the viewport at monitor refresh]
```

The engine side is a pipelined readback with zero added stalls. Each frame-in-flight slot
owns a BGRA8 image and a persistently mapped staging buffer; `endFrame` records the
offscreen→BGRA8 blit (the GPU does the format conversion) and the image→buffer copy into
the frame's normal command buffer, then submits with the frame fence only — no swapchain
acquire, no present, no `waitIdle`. When `beginFrame` waits that fence two frames later,
the readback is complete by construction and a `memcpy` publishes it into the shared
segment. The segment is grow-only with a 32-byte header (`magic, width, height, seq,
ringSlots, slotCapacity`) and a fixed-capacity 4-slot ring: frame `s` lands in slot
`s % 4`, the header is written pixels-first with `seq` bumped last behind a release fence,
so a reader that sees a new `seq` is guaranteed matching dimensions and pixels.

The editor side runs one worker thread that wraps GTK's own `wl_display` connection with a
private event queue, binds `wl_compositor`/`wl_subcompositor`/`wl_shm`/`wp_viewporter`,
and creates a **desync subsurface placed below** the toplevel. A `wl_shm_pool` wraps the
engine's segment directly — the compositor reads the very memory the engine wrote, one
copy end to end. The loop attaches the newest ring slot, damages, and commits, paced by
frame callbacks (one per monitor refresh) with a bounded self-paced fallback for the spans
when callbacks are withheld. `wp_viewport` scales the buffer to the panel's logical rect
and `set_position` pins it, both fed from the [viewport panel](../viewport-panel/)'s
bounds through a Tauri command.

## Load-bearing details

Three Wayland behaviours shape the implementation; missing any of them looks like "no
viewport at all" rather than an error.

- **Subsurface state is double-buffered on the parent.** Creation and `set_position` only
  take effect when the *toplevel* commits. A static transparent window may not be
  committing at all, so the presenter nudges `queue_draw` on bounds changes and while the
  worker comes up.
- **A fully transparent toplevel freezes GTK.** The compositor stops presenting a window
  with nothing visible, which starves GTK3's frame clock of callbacks and halts its paint
  loop — and with it the parent commits the subsurface needs. The window paints one
  near-invisible 2×2 dot in its draw handler so it always counts as visible, and clears
  its opaque region so the compositor blends below it.
- **Frame callbacks pace, they do not certify.** A callback per commit proves cadence, not
  that those pixels reached glass. `wp_presentation` feedback (counted per second behind
  `SAFFRON_VIEWPORT_STATS=1`) reports `presented`/`discarded` plus the vblank delta — and
  even `presented` only certifies the surface was in an on-screen repaint, so the eyeball
  test on fast motion stays part of verification.

Because the engine never presents, nothing throttles its loop; the editor caps it via
`SAFFRON_MAX_FPS` (default 500) so slots are not rewritten mid-read at thousands of fps.

> [!NOTE]
> On NVIDIA, WebKitGTK's default DMABUF renderer draws nothing under Wayland and its
> fallback loses transparency. The editor steers WebKit onto Mesa's software EGL
> (`__EGL_VENDOR_LIBRARY_FILENAMES` + `LIBGL_ALWAYS_SOFTWARE=1`), gated on NVIDIA being
> present so AMD/Intel keep the fast path. The engine itself still renders on the
> hardware ICD.

## Input rides the control plane

The engine's SDL window is hidden and receives no events, so every input path is a control
command from the webview: `gizmo-pointer` and `pick` for the [gizmo](../gizmo/) and
[selection](../selection/), and `fly-input` for the [editor camera](../editor-camera/)
(pointer-lock relative deltas + move keys). Webview pointer events arrive at ~60Hz, so the
engine smooths gizmo drag samples toward their target each rendered frame
(`stepNativeGizmoDrag`) instead of staircase-stepping at the sample rate.

> [!NOTE]
> wl_shm makes the compositor upload each frame on its paint thread, and the ring has no
> `wl_buffer.release` handshake. The planned cure is zero-copy linux-dmabuf buffers with a
> release-driven lifecycle — see `plans/dmabuf-viewport/`.

## In the code

| What | File | Symbols |
|---|---|---|
| Publish state + slots | `renderer_types.cppm` | `ShmPublish`, `ShmPublishSlot` |
| Slot lifecycle + segment | `renderer_capture.cpp` | `enableViewportShmPublish`, `ensureShmPublishSlot`, `publishShmPublishSlot`, `destroyShmPublish` |
| Recorded readback + fence-only submit | `renderer.cppm` | `recordShmPublishCopy`, the `shmPublish` branches in `beginFrame`/`endFrame` |
| Loop cap | `app.cppm` | `maxFpsFromEnv` |
| Subsurface presenter | `editor/src-tauri/src/wayland_viewport.rs` | `install`, `run`, `ViewportShared`, `PresentationStats` |
| Rect + park bridge | `editor/src-tauri/src/lib.rs` | `set_viewport_bounds`, `set_viewport_hidden`, `spawn_engine` |
| Render size command | `control_commands_render.cpp` | `set-viewport-size` |

## Related

- [Tauri editor and the viewport bridge](../tauri-editor-and-viewport-bridge/) — the shell and control passthrough around this transport
- [Viewport panel](../viewport-panel/) — the rect, input forwarding, and parking
- [Editor camera](../editor-camera/) — the fly input streamed over `fly-input`
- [Control plane](../../tooling-and-control/control-plane-architecture/) — the socket the input and size commands ride
