# Phase 2 — linux-dmabuf buffers end to end

**Status:** NOT STARTED

Replace the shm pixel path with GPU buffers — the same mechanism XWayland uses to feel
native (glamor wraps swapchain pixmaps via linux-dmabuf `create_immed` in
`xwayland-glamor-gbm.c`). Three wins: zero-copy (the compositor samples engine-rendered
memory, no upload), direct-scanout candidacy, and a `wl_buffer.release`-driven lifecycle
that closes the ring overwrite race for good. Keep the wl_shm path as the fallback when
dmabuf feedback or binding fails.

## Engine side (`renderer_capture.cpp` / `renderer.cppm` / `renderer_types.cppm`)

- Allocate 3-4 **publish images** as `B8G8R8A8`/XRGB-compatible with
  `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` (`VK_EXT_image_drm_format_modifier`):
  `VkImageDrmFormatModifierListCreateInfoEXT` carries the modifiers the editor relays from
  compositor feedback; `DRM_FORMAT_MOD_LINEAR` is the safe fallback. Memory is exportable
  (`VkExportMemoryAllocateInfo` with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`,
  dedicated allocation).
- Export each once: `vkGetMemoryFdKHR` (dma_buf handle type), then query
  `vkGetImageDrmFormatModifierPropertiesEXT` and per-plane `vkGetImageSubresourceLayout`
  (with `VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT`) for offset/stride.
- The per-frame blit in `recordShmPublishCopy` targets one of these images instead of the
  staging-buffer path (keep the blit — it is the format conversion; drop
  `copyImageToBuffer`, the memcpy, and the shm segment on this path).
- **Buffer lifecycle:** render only into images the editor reports RELEASED; track
  per-image busy/free from the relayed `wl_buffer.release` events.

## Transport (control socket)

- fd passing is `SCM_RIGHTS` ancillary data over the existing unix socket. New commands:
  `viewport-dmabuf-info` (editor→engine: the compositor's format/modifier tranches from
  `get_surface_feedback`) and a one-time engine→editor handoff of N fds +
  `{width, height, fourcc DRM_FORMAT_XRGB8888, modifier, plane offsets/strides}`.
  Re-handoff on resize.
- Release/acquire signalling, simplest v1: a tiny shared header (reuse the shm segment
  header area) with per-buffer busy flags the editor updates from `wl_buffer.release`;
  the engine polls before rendering into a buffer.

## Editor side (`wayland_viewport.rs`)

- Bind `zwp_linux_dmabuf_v1` (≥ v4), `get_surface_feedback` on the subsurface for the
  format/modifier tranches; relay to the engine.
- Receive fds; build wl_buffers: `create_params` → `add(fd, plane, offset, stride,
  modifier_hi, modifier_lo)` → `create_immed(w, h, fourcc, 0)`.
- Commit the newest **released** buffer per frame callback (the existing pacing loop
  stays); handle `wl_buffer.release` per buffer and relay to the engine.

## Explicit sync (with phase 2 if straightforward, else fast-follow)

`linux-drm-syncobj-v1` (mutter ≥ 46, NVIDIA ≥ 555). NVIDIA's egl-wayland2 notes that
without explicit sync the result is "reduced performance and out-of-order frames". The
engine exports a timeline syncobj; acquire point = render-done; the editor sets
acquire/release points per commit via `wp_linux_drm_syncobj_surface_v1`.

## Verify

- `SAFFRON_VIEWPORT_STATS=1`: `zero-copy` appears in the flags; presented/s stays at the
  monitor rate; discarded stays ~0.
- Tearing check: fast engine-driven motion shows no horizontal shear with the engine
  uncapped (the race is closed by the release lifecycle, not the fps cap).
- Side-by-side feel vs the standalone present path.

## References

- linux-dmabuf: <https://wayland.app/protocols/linux-dmabuf-v1>
- explicit sync: <https://wayland.app/protocols/linux-drm-syncobj-v1>
- presentation-time: <https://wayland.app/protocols/presentation-time>
- `VK_EXT_external_memory_dma_buf` / `VK_EXT_image_drm_format_modifier` (Khronos registry)
- mutter per-buffer texture cache: <https://gitlab.gnome.org/GNOME/mutter/-/issues/199>
- shm-is-slow (KWin dev): <https://zamundaaa.github.io/wayland/2026/05/06/making-wl-shm-fast.html>
- NVIDIA dmabuf client stack: <https://github.com/NVIDIA/egl-wayland2>
- XWayland dmabuf wrap: xserver `hw/xwayland/xwayland-glamor-gbm.c`
