# Phase 1 — GPU downscale before readback

**Status:** COMPLETED

The single biggest win. `encodeTextureThumbnailPng` (`renderer_thumbnail.cpp:672-734`)
ignores its `size` parameter (`renderer_thumbnail.cpp:675`) and reads the texture back at
native extent, so a 4k HDR costs a 64 MB readback plus a full-resolution stb PNG encode
and a multi-MB base64 reply. Mesh and material thumbnails already render *into* a
size×size target (`renderMeshThumbnail`, `renderMaterialPreview`) — only the plain-texture
path is wrong. After this phase a 128 px texture thumbnail reads back ~128 KB and encodes
~16k pixels regardless of source resolution; `view-asset` (512) gets the same treatment
through the same `size` parameter.

## The work

- In `encodeTextureThumbnailPng`, when the texture's max dimension exceeds `size`, blit it
  down on the GPU before `captureImageToBuffer`:
  - Target extent: fit within size×size preserving aspect (max dimension = `size`), so the
    editor's `object-contain` display keeps working unchanged.
  - Create a transient image in the *same format* as the source (RGBA16F sources stay
    RGBA16F), usage `TRANSFER_DST | TRANSFER_SRC`, then `vkCmdBlitImage` with
    `vk::Filter::eLinear` and read *that* back. Same-format blits keep `convertToRgb`
    (`renderer_detail.cppm:1209`) working as today; tonemapping changes are phase 2.
  - A single 4096→128 blit undersamples (linear filter reads 2×2 texels). Do a chained
    halving blit (repeated 2× reductions into two ping-pong transients, or successive
    regions of one image) down to the target — the standard mip-style reduction. Sources
    already carry `TRANSFER_SRC` (`renderer_textures.cpp:378-380`).
  - Source layout is `eShaderReadOnlyOptimal` and must be restored (the bindless array
    stays valid — same constraint `captureImageToBuffer` already honours at
    `renderer_thumbnail.cpp:709-714`).
- Record the blit chain and the buffer copy in the one existing command buffer/submit; no
  extra submits or waits beyond what is already there (wait reduction is phase 6).
- Report the *actual* PNG dimensions in `ThumbnailResult`: `thumbnailResult`
  (`control_commands_asset.cpp:404`) currently claims `size`×`size` whatever the PNG is.
  Return the blitted width/height (and the rendered size for mesh/material). DTO shape is
  unchanged — only the values become truthful.

## Verification

- e2e (`tests/e2e/`): import a texture fixture larger than 128, `get-thumbnail`, decode
  the base64 PNG header and assert max dimension == 128 and the reply's width/height match
  the PNG. Repeat with `view-asset` (512). Existing `material_thumbnail.test.ts` /
  `preview_render.test.ts` show the harness shape.
- Manual: `make run` against a project with the 4k HDR — the tile appears without a
  multi-second viewport freeze, and the reply no longer trips the editor's 5 s timeout.
- Milestone gate: `make engine` + `make prepare-for-commit`; validation-clean headless run.
- Docs: update the thumbnail-transport paragraphs in
  `docs/content/explanations/ui-and-editor/assets-panel-and-thumbnails.md` (and
  `mesh-thumbnails.md` if its readback description changes).
