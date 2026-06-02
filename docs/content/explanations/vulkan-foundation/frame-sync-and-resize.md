+++
title = 'Frame sync'
weight = 6
+++

# Frame sync

Frame synchronization is the set of fences and semaphores that lets the CPU record one frame while the GPU renders another without either overwriting the other's work. The CPU never resets a command buffer the GPU is still reading, and a swapchain image is never reused before its previous present completes.

The renderer keeps two frames in flight and coordinates them with a per-frame ring of sync objects plus a per-image fence. Window and viewport resize are handled as ordinary swapchain and target recreation, gated on a device idle.

## How it works

`MaxFramesInFlight` is 2. The renderer holds an `std::array<FrameData, 2>` and a rotating `index`. Each `FrameData` is the CPU-side recording context for one in-flight frame:

```cpp
struct FrameData
{
    vk::CommandPool commandPool;
    vk::CommandBuffer commandBuffer;
    vk::Semaphore imageAvailable;
    vk::Fence inFlight;
};
```

The `imageAvailable` semaphore and `inFlight` fence belong to the *frame slot*. The `renderFinished` semaphores belong to the *swapchain image* — one per image, not per frame. Present waits on "this image's rendering is done," and an image can be presented across different frame slots.

## Acquiring a frame

`beginFrame` runs the CPU-side waits and image acquisition:

1. **Wait on the frame fence.** `waitForFences(frame.inFlight, ...)` blocks until the GPU finishes the work this slot submitted two frames ago, freeing its command buffer to reset and re-record.
2. **Acquire the next swapchain image**, signaling `frame.imageAvailable`. `eErrorOutOfDateKHR` rebuilds the swapchain and skips the frame; `eSuboptimalKHR` is accepted and rendered.
3. **Wait on the image's own fence if it's still in flight.** `imagesInFlight` records which frame fence last used each image. The swapchain image count need not equal `MaxFramesInFlight`, so the acquired image might still be referenced by an earlier frame. Waiting on its fence before reusing its `renderFinished` semaphore avoids signaling a semaphore that is still pending.
4. **Reset the fence and command buffer**, clear the frame's draw list and submission vectors, and `begin()` with `eOneTimeSubmit`.

## Submitting and presenting

`endFrame` records the UI pass, runs the graph, ends the command buffer, and submits with `submit2`. The submit uses a binary-semaphore handshake: **wait** on `frame.imageAvailable` at `eColorAttachmentOutput` (don't write color until the image is acquired), **signal** the image's `renderFinished` at `eAllCommands`, and **fence** `frame.inFlight` so this slot can wait next time around.

`presentKHR` waits on `renderFinished` and queues the image. An out-of-date or suboptimal present triggers `recreateSwapchain`. The slot then advances `index = (index + 1) % MaxFramesInFlight`.

```mermaid
flowchart TD
    A[beginFrame] --> B[wait frame.inFlight fence]
    B --> C[acquireNextImageKHR<br/>signals imageAvailable]
    C --> D[wait imagesInFlight fence]
    D --> E[reset fence + cmd buffer; begin]
    E --> F[record passes]
    F --> G[endFrame: submit2<br/>wait imageAvailable, signal renderFinished, fence inFlight]
    G --> H[presentKHR waits renderFinished]
    H --> I[index = index+1 mod 2]
    I --> A
```

## Resize

Two resizes happen, both in `beginFrame`, both gated on a device idle. A full `device.waitIdle()` is the simple, correct choice: the offscreen is a single shared target, nothing may still be reading it, and resizes are rare (a dragged panel edge), so the stall is acceptable.

**Swapchain resize.** When the window extent differs from the swapchain extent, `beginFrame` idles the device, calls `recreateSwapchain`, and returns `false` to skip the frame. `recreateSwapchain` bails on a zero extent (minimized) and otherwise rebuilds, passing the old swapchain as `set_old_swapchain` so the driver can recycle it. Per-image views and `renderFinished` semaphores are recreated to match.

**Viewport resize.** The editor's Viewport panel can differ in size from the window and drives `desiredWidth/Height`. When those differ from the offscreen extent, `beginFrame` idles and recreates the offscreen color, depth, and every dependent target — MSAA, FXAA/TAA scratch, the SSAO G-buffer and AO maps, TAA motion + history, and (when supported) ReSTIR reservoirs — then bumps a `generation` counter so the UI knows the [viewport descriptor](../../frame-and-render-graph/cross-frame-layouts/) must refresh.

## Why a per-image fence on top of per-frame fences

The per-frame fence alone guarantees the *slot's* command buffer is safe to reuse. It does not guarantee the *image* is free. With 3 swapchain images and 2 frames in flight, a just-acquired image might have last been used by the other slot. Reusing its `renderFinished` semaphore before that work finishes is a validation error. The `imagesInFlight` array closes the gap by tracking the last frame fence per image and waiting on it.

## In the code

| What | File | Symbols |
|---|---|---|
| Frames-in-flight constant | `renderer_types.cppm` | `MaxFramesInFlight` |
| Per-frame sync objects | `renderer_types.cppm` | `FrameData`, `FrameSync` |
| Per-image fences/semaphores | `renderer_types.cppm` | `Swapchain::imagesInFlight`, `renderFinished` |
| Acquire + waits | `renderer.cppm` | `beginFrame` |
| Submit + present + advance | `renderer.cppm` | `endFrame`, `submit2`, `presentKHR` |
| Swapchain rebuild | `renderer_detail.cppm` | `recreateSwapchain`, `buildSwapchain` |

> [!NOTE]
> `acquireNextImageKHR` and `presentKHR` returning `eSuboptimalKHR` or `eErrorOutOfDateKHR` are not errors — they mean "rebuild the swapchain." Both sites branch on the result directly instead of going through `checked()`, which would turn any non-`eSuccess` into an `Err`.

## Related

- [Device & swapchain](../device-and-swapchain/) — how the swapchain + its semaphores are built
- [Cross-frame layouts](../../frame-and-render-graph/cross-frame-layouts/) — the offscreen layout + generation counter carried across frames
- [No-exceptions Vulkan-Hpp](../vulkan-hpp-no-exceptions/) — why acquire/present skip `checked`
- [Render graph overview](../../frame-and-render-graph/render-graph-overview/) — recorded between begin and endFrame
