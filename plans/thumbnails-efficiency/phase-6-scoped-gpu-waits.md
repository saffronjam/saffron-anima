# Phase 6 — scoped GPU waits

**Status:** NOT STARTED

Groundwork for phase 7, and a win on its own. The thumbnail/upload paths call
`device.waitIdle()` after every submit — draining the *entire* GPU, including the in-flight
scene frame, when they only need their own one-off command buffer to finish:

| Site | Wait |
|---|---|
| `renderer_textures.cpp:265` (`uploadTexture`) | after upload submit |
| `renderer_textures.cpp:428` (`uploadTextureFloat`) | after upload submit |
| `renderer_thumbnail.cpp:314` (`renderMeshThumbnail`) | after render submit |
| `renderer_thumbnail.cpp:655` (`renderMaterialPreview`) | after render submit |
| `renderer_thumbnail.cpp:684` (`encodeTextureThumbnailPng`) | *before* readback submit |
| `renderer_thumbnail.cpp:722` (`encodeTextureThumbnailPng`) | after readback submit |

A worker thread (phase 7) cannot call `device.waitIdle()` while the main thread submits
frames, so these must become per-submit waits first.

## The work

- Replace each post-submit `waitIdle` with a `vk::Fence` passed to `submit2` and a
  `waitForFences` on just that submission. Wrap in `checked(...)` per the renderer rules.
- Audit the pre-readback wait at `renderer_thumbnail.cpp:684`: the capture is a
  transfer-*read* of a texture the frame only ever samples (both reads, same
  `eShaderReadOnlyOptimal` layout, barriers inside the capture command buffer handle the
  transfer hazard). If analysis confirms no write hazard exists, drop it; if a real
  ordering against in-flight frame work is needed, replace it with a wait on the frame's
  existing per-frame fence rather than a device drain. Document the conclusion in the
  code.
- These one-off command buffers all allocate from `renderer.frame.frames[0].commandPool`
  (e.g. `renderer_thumbnail.cpp:695`); note for phase 7 that a worker needs its own pool —
  no behaviour change in this phase.

## Verification

- Validation-clean headless run (`SAFFRON_EXIT_AFTER_FRAMES` smoke + `make e2e`): sync
  validation is the real test here — a wrong narrowing shows up as hazards reported by the
  validation layers, which the e2e suite asserts clean.
- Manual: thumbnails still correct under a moving viewport (generate while orbiting).
- Milestone gate: `make engine` + `make prepare-for-commit`.
