# Phase 11 — Studio IBL + material preview pass

**Status:** NOT STARTED
**Depends on:** 05

## Goal

Engine-side material preview: bake a dedicated **studio IBL** once at startup (from a shipped HDR), and
add an **in-frame render-graph pass** that renders a sphere with a candidate material using the **real
`mesh.slang`** into a small offscreen target, read back to a buffer. This is the WYSIWYG preview the
editor (phase 13) displays. The flat `thumbnail.spv` pipeline is **not** usable (it binds zero descriptor
sets and cannot do PBR/IBL) — it is reused only for the readback→PNG tail.

## Why

A material preview must match the final look, so it must use the actual übershader + lighting. The
present-only host renders the scene through the deferred submit seam inside `endFrame`'s render graph;
a control-command handler runs **before** `beginFrame`. So the preview can't call `renderScene` from a
handler — instead, the host arms a flag and the preview pass runs **in-frame** alongside the main pass,
reading back the result for the next control response.

## Design

- **Studio IBL**: ship a small neutral studio `.hdr` under `engine/assets/`. At startup, bake a *separate*
  `Ibl` instance via `bakeEnvironment(renderer, studioParams, true)` (irradiance + prefiltered + BRDF LUT).
  Bind this set 3 for the preview pass instead of the scene's `renderer.ibl` (which may be black/mid-rebake).
- **Preview scene state** (host): a unit-sphere `GpuMesh`, a transient `DrawItem` carrying the candidate
  material's resolved `SubmeshMaterial`, a fixed camera, and a synthetic globals/lights UBO: one key
  directional light, `counts.z=1` (IBL on, studio), `counts.x=0` (no punctual), `counts.y=0`/`screenFlags=0`
  (no shadow/SSAO/SSGI/ReSTIR/DDGI). Sets 4/5(/6/7) that the PSO layout requires but the branches won't
  sample get **dummy** bindings (1×1 images / empty buffers) — they must be valid, just unused.
- **The pass**: a `RgPass{kind:Graphics, color: previewTarget, depth: previewDepth, execute: …}` added in
  `beginFrameGraph` **when a preview is armed**. It binds the mesh pipeline (`requestMeshPipeline`), the
  bindless set (real material textures), the synthetic globals + studio IBL + dummies, and draws the sphere.
- **Readback**: after the pass, `captureImageToBuffer(previewTarget)` into a staging buffer; the host
  surfaces it to the control layer (PNG encode in phase 12).

## Files to touch

- `engine/assets/` — a shipped studio `.hdr` (copied next to the exe by CMake, like other assets).
- `engine/source/saffron/rendering/renderer_types.cppm` — a `Targets`-adjacent `previewColor`/`previewDepth`
  + a second `Ibl studioIbl`.
- `engine/source/saffron/rendering/renderer.cppm` — bake `studioIbl` at startup; in `beginFrameGraph`, add
  the preview pass when armed; dummy descriptors for unused sets.
- `engine/source/saffron/rendering/` — a `renderMaterialPreview`-style helper that records the sphere draw
  with the synthetic globals (reuse the scene-pass recording code paths where possible).
- `engine/source/saffron/host/host.cppm` — preview scene state (sphere mesh, armed material), arm/disarm
  from a control command (phase 12), and the readback handoff.

## Steps

1. Ship + bake the studio IBL into a separate `Ibl`; verify it samples (a chrome sphere shows the studio).
2. Add the preview color/depth targets + a unit sphere mesh.
3. Build the synthetic globals/lights UBO + dummy bindings for unused sets; record a sphere draw with the
   real mesh pipeline + a candidate `SubmeshMaterial`.
4. Wire the in-frame pass behind an "armed" flag; read back to a staging buffer.
5. Test: arm a known material, dump the readback to PNG, eyeball a correctly-lit PBR sphere.

## Gate / done

- `make engine` clean; an armed material renders a correctly-lit sphere (matches how it looks in-scene).
- The main scene render is unaffected when no preview is armed (zero added cost on the common path).
- `make prepare-for-commit` clean. Docs: the preview rendering concept.

## Risks

- **Descriptor completeness**: Vulkan requires all sets in the PSO layout to be bound with valid
  descriptors even if the shader branches past them. Provide real 1×1 dummies; null descriptors will
  validation-error. This is the single most likely source of validation failures — budget for it.
- **IBL bake cost**: `bakeEnvironment` calls `waitIdle` internally; do it once at startup, never per request.
- **Threading/lifetime**: the preview material's `Ref<GpuTexture>`s must be pinned for the frame (like
  `SceneDrawList.liveTextures`); drop them after readback.
