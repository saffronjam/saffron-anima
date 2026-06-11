# Phase 02 — Material params buffer

**Status:** NOT STARTED
**Depends on:** 01

## Goal

Introduce a `StructuredBuffer<MaterialParams>` at **set 2, binding 2**, give `InstanceData` a
single `materialIndex` (packed into the free `texture.w` lane), and have `submitDrawList` dedup
resolved `SubmeshMaterial`s into a per-frame material table. Move the existing albedo/MR indices
and scalar factors out of the instance and into `MaterialParams`. **Same visuals.** This is the
data plumbing every later phase writes through, and it scales to the arbitrary parameter counts
node-graph materials need (which can never fit a fixed instance struct).

> **Heads-up (verified phase 01):** the shader `Instance` struct in `mesh.slang` has a **`prevModel`
> mat4** lane (TAA object-motion) that the initial grounding missed — so it is 3× mat4, not 2×.
> **Re-verify the real `InstanceData` layout in `renderer_types.cppm` before editing** (it likely
> includes `prevModel`, making it 256 B not 192 B). The `materialIndex` still goes in `texture.w`,
> but confirm the CPU/GPU field order (incl. `prevModel`) and update the `static_assert` accordingly.

## Why

`InstanceData` is 192 B with only `texture.w`/`pbr.zw`/`emissive.w` free — enough for nothing
much, and a dead end for node-graph materials. UE/Unity both put per-material data in a material
buffer indexed by the instance. Dedup is also the edit-once-propagate mechanism shared `.smat`
assets want: N instances of one material → one `MaterialParams` entry.

## The layout

```cpp
// renderer_types.cppm — std430, 16-byte aligned. Grows in phases 05/06; reserve generously.
struct MaterialParamsData
{
    glm::vec4 baseColor{ 1.0f };
    glm::vec4 pbr{ 0.0f, 1.0f, 0.0f, 0.5f };   // metallic, roughness, normalStrength, alphaCutoff
    glm::vec4 emissive{ 0.0f };                // rgb radiance, .w = heightScale
    glm::vec4 uv{ 1.0f, 1.0f, 0.0f, 0.0f };    // tiling.xy, offset.xy
    glm::uvec4 tex0{ 0u };                      // albedo, ormOrMr, normal, emissive  (bindless)
    glm::uvec4 tex1{ 0u };                      // height, (reserved), (reserved), featureBits
};
```

```slang
// mesh.slang — mirror exactly.
struct MaterialParams { float4 baseColor; float4 pbr; float4 emissive; float4 uv; uint4 tex0; uint4 tex1; };
[[vk::binding(2, 2)]] StructuredBuffer<MaterialParams> materialParams;
```

`InstanceData.texture.w` = `materialIndex`. In `vertexMain`/`vertexMainSkinned`, pass
`inst.texture.w` through to `VertexOutput.materialIndex`; in `fragmentMain`, do
`MaterialParams mp = materialParams[input.materialIndex];` and build `MaterialInput` from `mp`
instead of the per-instance lanes. This phase only wires albedo/mr/factors through `mp`; the
extra `tex0.zw`/`tex1`/`uv` lanes stay zero until phase 05.

## Files to touch

- `engine/source/saffron/rendering/renderer_types.cppm` — add `MaterialParamsData`; keep `InstanceData`
  (repurpose `texture.w` as materialIndex; the now-redundant albedo/mr/pbr/emissive lanes can stay for
  one phase then be trimmed, or trim now — your call, but the shader must read from `materialParams`).
- `engine/source/saffron/rendering/renderer_drawlist.cpp` — in `submitDrawList`: build a
  `std::vector<MaterialParamsData>` with a dedup map (key on the `SubmeshMaterial`'s resolved indices +
  factors), set each instance's `materialIndex`, upload the table to a per-frame buffer.
- `engine/source/saffron/rendering/renderer_pipelines.cpp` — add binding 2 to `instanceSetLayout`
  (a `StorageBuffer`), and write the material buffer into `instanceSet` each frame.
- `engine/source/saffron/rendering/` (instancing/descriptors) — allocate + grow the material buffer
  alongside the instance/joint buffers (same persistent-mapped, power-of-2 grow pattern).
- `engine/assets/shaders/mesh.slang` — add `MaterialParams` + binding; read `mp` and feed `evalSurface`.

## Steps

1. Define `MaterialParamsData` (CPU) + `MaterialParams` (shader), identical layout — verify with a
   `static_assert(sizeof(MaterialParamsData) == 96)`.
2. Add set-2 binding 2 to the instance set layout + per-frame buffer (mirror the joint buffer code).
3. In `submitDrawList`, dedup `SubmeshMaterial`→`MaterialParamsData`, fill `materialIndex`, upload.
4. Shader: read `materialParams[materialIndex]`, build `MaterialInput`, call `evalSurface`.
5. Smoke: identical frame.

## Gate / done

- `make engine` clean; present-only smoke identical to phase 01.
- `make prepare-for-commit` clean.
- `static_assert` guards the CPU/GPU layout match.

## Risks

- **Layout mismatch** between CPU `MaterialParamsData` and the Slang struct — the static_assert plus
  a deliberate test value (set a known baseColor, read it back via a debug present) catches it.
- **Dedup key** must include everything that distinguishes a material (indices + all factors) or
  distinct materials collapse. Hash the lowered struct's bytes, or compare field-wise.
- Descriptor set 2 now has 3 bindings; ensure `maxBoundDescriptorSets` and pool sizes still fit.
