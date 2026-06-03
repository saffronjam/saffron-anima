# Phase 1 — Windowless offscreen viewport + async readback ring

**Status:** NOT STARTED

**Depends on:** —

## Goal

Make the engine able to run the viewport with **no swapchain and no native window at all**:
render the scene into `targets.offscreen` exactly as today, then expose the latest frame as a
host-readable `RGBA8` buffer via an **async, non-stalling** readback ring — instead of
blitting to a swapchain image and presenting to a reparented X11 window. The legacy X11 path
stays intact behind the existing present-only flag so nothing breaks mid-migration.

## Why this is mostly wiring

The render already terminates in an offscreen image and only *then* reaches the window:

- `renderer.targets.offscreen` (`renderer_types.cppm`, `OffscreenColorFormat`) is the final
  post-tonemap color image.
- `presentViewportToSwapchain` (`renderer.cppm:1565`) blits offscreen → swapchain →
  `ePresentSrcKHR`, and `presentKHR` (`renderer.cppm:1718`) shows it. **This is the only hop
  we remove.**
- `captureViewport` (`renderer_capture.cpp:38`) already does offscreen → host buffer via
  `captureImageToBuffer` + `newHostCaptureBuffer` (`renderer_detail.cppm:923`). It is
  synchronous (`device.waitIdle`, `renderer_capture.cpp:97`) — fine for a one-off screenshot,
  not for every frame.

## Steps

1. **Add a `viewportTransport` mode** alongside `presentViewportOnly`
   (`renderer.cppm`/`renderer_types.cppm`): `Swapchain` (today) | `ReadbackRing` (new) |
   later `Dmabuf` (phase 4). Selected by the host (see step 6). In `ReadbackRing` mode the
   renderer builds **no swapchain and acquires no surface** — guard `buildSwapchain`
   (`renderer_detail.cppm:104`), the `vkAcquireNextImageKHR`/`presentKHR` calls
   (`renderer.cppm:573`,`:1718`), and `presentViewportToSwapchain` so they are skipped.

2. **Resolve offscreen `R16F` → `RGBA8` sRGB** into a per-ring image. Reuse the same
   conversion the swapchain blit performs (`vkCmdBlitImage`, Nearest, format change), but
   target an `RGBA8_UNORM`/`SRGB` image instead of a `B8G8R8A8` swapchain image. Confirm
   premultiplied-alpha + sRGB encoding (Open Question #3).

3. **Async readback ring (N = 2–3 frames).** Per ring slot: a host-visible mapped buffer
   (`newHostCaptureBuffer`) + its own fence. Each frame: record `captureImageToBuffer` into
   slot `i = frame % N`, signal that slot's fence; **before reading, wait the slot you are
   about to overwrite, not the one just written** — i.e. read slot `i-1` whose copy completed
   a frame ago, never `device.waitIdle`. This pipelines copy(frame n) with read(frame n-1).

4. **Publish the latest frame** as `{ seqno, width, height, format, stride, ptr }` for the
   transport layer (phase 2): a small struct + the mapped pointer of the most-recently-ready
   slot, with a monotonically increasing `seqno`. No copy here — the transport owns moving
   bytes out.

5. **Frame size negotiation.** Keep `setViewportDesiredSize`
   (`control_commands_render.cpp:394`,`:442`) as the size input; `beginFrame`
   (`renderer.cppm:597`) already resizes `targets.offscreen` on a desired-size change — the
   ring images resize with it. No window geometry is involved anymore.

6. **Host wiring** (`host.cppm`): when the editor requests the readback transport (env or a
   control command, phase 3), call `setPresentViewportOnly(true)` **and** select
   `ReadbackRing`; the SDL window stays hidden and unused (`host.cppm:344`,`:357`). The main
   loop (`run`) keeps `pollControl` (`host.cppm:459`) and the per-frame render, minus present.

## Validation

- `SAFFRON_EXIT_AFTER_FRAMES=N` headless run in `ReadbackRing` mode: validation-clean log, no
  swapchain/surface objects created (assert via a Vulkan validation layer check or a log
  line), `seqno` advances once per frame.
- Dump frame 10's buffer to PNG (reuse `writeBufferToPng`) and eyeball it matches the
  swapchain-path output of the same scene.
- Measure and log per-frame readback time (feeds Open Question #1).
- Legacy `Swapchain` mode (`make run` today) still attaches and presents unchanged.

## Risks

- **GPU→CPU stall** if the ring waits on the just-recorded copy. Mitigation: the N-slot
  read-previous discipline in step 3; verify no per-frame `waitIdle` survives.
- **Format/sRGB mismatch** showing washed-out or too-dark frames in the canvas — pin down in
  step 2 with a known test pattern before phase 2 consumes it.
