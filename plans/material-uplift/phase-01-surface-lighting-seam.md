# Phase 01 — Surface/lighting seam

**Status:** COMPLETED
**Depends on:** —

> **Outcome:** `SurfaceData`/`MaterialInput`/`evalSurface` added to `mesh.slang`; `fragmentMain`
> now builds a `MaterialInput`, calls `evalSurface`, and feeds `surf.*` into the unchanged
> lighting (incl. `ambient *= surf.occlusion`, currently 1.0). `slangc` compiles clean (only the
> pre-existing profile-upgrade warning). Math is byte-equivalent. Note: the headless present-only
> smoke aborts at **teardown** on a pre-existing VMA "allocations not freed" assertion — unrelated
> to this shader change (a shader edit cannot leak GPU allocations); the pipeline loads and renders
> to teardown without per-frame validation errors. Teardown leak tracked as a separate, not-mine issue.

## Goal

Refactor `mesh.slang`'s `fragmentMain` so the surface-evaluation section becomes a single
function `SurfaceData evalSurface(MaterialInput)`, and the lighting section consumes its
result. **Zero behaviour change** — the rendered image must be pixel-identical. This is the
keystone seam: feature bits (phase 05/06) extend `evalSurface`, and the node graph (phase 18)
*generates* its body. The lighting half is never touched again.

## Why

`fragmentMain` today is two halves glued together: surface (≈L466–480, 537: sample albedo,
multiply baseColor, sample metallic-roughness, normalize the geometric normal) and lighting
(≈L481–602: directional + clustered/ReSTIR punctual + IBL + probes + SSAO + SSGI + DDGI). Only
the second half is the expensive, shared übershader. Splitting them is good hygiene regardless
and is the only structural change the node-graph endgame strictly requires.

## The ABI (define these in `mesh.slang`)

```slang
struct SurfaceData
{
    float3 albedo;     // linear, pre-baseColor-multiplied
    float  metallic;
    float  roughness;  // clamped away from 0
    float3 normal;     // world-space, normalized (perturbed later by normal maps)
    float3 emissive;   // additive radiance
    float  occlusion;  // 1.0 = none; multiplies the ambient term only
    float  opacity;    // alpha; 1.0 today
};

struct MaterialInput
{
    float3 worldPos;
    float3 worldNormal;   // interpolated geometric normal
    float2 uv0;
    float3 viewDir;       // normalize(eye - worldPos)
    float4 baseColor;     // from the instance/material
    uint   albedoIndex;   // bindless
    uint   mrIndex;       // bindless
    float2 mrFactor;      // metallic, roughness factors
    float3 emissiveIn;
    // grows in phase 05 (normal/orm/emissive-tex indices, tiling, feature bits)
};
```

`evalSurface` for now does exactly what L466–480/537 do today:

```slang
SurfaceData evalSurface(MaterialInput m)
{
    SurfaceData s;
    float4 tex = albedoTextures[NonUniformResourceIndex(m.albedoIndex)].Sample(m.uv0);
    s.albedo = tex.rgb * m.baseColor.rgb;
    s.opacity = tex.a * m.baseColor.a;
    float4 mr = albedoTextures[NonUniformResourceIndex(m.mrIndex)].Sample(m.uv0);
    s.metallic = saturate(m.mrFactor.x * mr.b);
    s.roughness = clamp(m.mrFactor.y * mr.g, 0.045, 1.0);
    s.normal = normalize(m.worldNormal);
    s.emissive = m.emissiveIn;
    s.occlusion = 1.0;
    return s;
}
```

`fragmentMain` builds a `MaterialInput` from `VertexOutput`, calls `evalSurface`, and feeds
`s.albedo/metallic/roughness/normal/emissive` into the existing lighting code (replace the
local `albedo`/`metallic`/`roughness`/`n` with `s.*`). The `kUnlit` early-out becomes
`return float4(s.albedo + s.emissive, s.opacity)`. The ambient term multiplies by `s.occlusion`
(today always 1.0, so no change).

## Files to touch

- `engine/assets/shaders/mesh.slang` — add `SurfaceData`/`MaterialInput`/`evalSurface`; rewrite
  the head of `fragmentMain` to call it. No CPU-side change. Slang fns must be declared before use.
- (No `renderer_types.cppm` change — `InstanceData` is unchanged this phase.)

## Steps

1. Add the two structs + `evalSurface` above the BRDF helpers (or just above `fragmentMain`).
2. Rewrite `fragmentMain`'s first ~15 lines to populate `MaterialInput` and call `evalSurface`.
3. Route `s.*` through the lighting code; keep `kUnlit` and the ambient `* s.occlusion`.
4. Rebuild shaders (CMake compiles `*.slang`→SPIR-V) and run a present-only smoke; the frame
   must look identical to before. A captured PNG (`captureViewport`) before/after should match.

## Gate / done

- `make engine` clean; present-only smoke renders identically (spot-check a captured frame).
- `make prepare-for-commit` clean.
- No `se`/docs change (internal refactor, no concept change).

## Risks

- **Behaviour drift** from reordering math (e.g. the roughness clamp, the `mr` default-white
  fallback). Keep the surface code byte-for-byte equivalent; this phase is a pure extraction.
- Slang struct-return codegen is fine, but confirm `evalSurface` inlines (it will at -O); if a
  perf regression shows up it's a red flag that something else changed.
