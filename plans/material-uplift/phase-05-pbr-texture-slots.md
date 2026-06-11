# Phase 05 — PBR texture slots

**Status:** COMPLETED
**Depends on:** 02, 04

> **Outcome / scope note.** Landed the full **entity→shader consumption path** for PBR slots, but the
> **populators + the visual relief test are deferred to phase 08** (which imports the coast-rocks normal
> map) — there is no normal-mapped fixture to test against yet, so verification here is no-regression.
> Done: `SubmeshMaterial` + `MaterialComponent` + `MaterialSlot` gained `normal/occlusion/emissive`
> textures + `normalStrength` + `uvTiling`/`uvOffset`; `resolveEntityMaterials` loads them; the drawlist
> derives the feature bits from texture presence and packs everything into `MaterialParamsData`
> (`tex0.z`=normal, `tex0.w`=emissive, `tex1.y`=occlusion, `pbr.z`=normalStrength, `uv`=tiling/offset,
> `tex1.w`=features). `evalSurface` now takes `MaterialParams`, applies the UV transform, and feature-gates
> normal mapping (`perturbNormal` + the phase-04 derivative TBN), occlusion (AO in R), and the emissive map.
> **Deferred:** glTF normal/occlusion/emissive **import** + the suffix importer populate these slots (phase
> 08); the new component fields are **in-memory only** — serde persistence is added with the component work
> in phase 09. Build clean; 6/6 material e2e no-regression (all current assets have `features=0` → identical).

## Goal

Make a `.smat` express a real PBR surface: add **normal map** (tangent-space), **packed ORM**
(AO+roughness+metallic, with channel routing), **emissive texture**, plus `normalStrength`,
UV tiling/offset, and a **feature bitset**. Extend `SubmeshMaterial`, `MaterialParamsData`/`MaterialParams`,
and `evalSurface` to sample and apply them.

## Why

This is the payload of the whole uplift — without these slots, materials are albedo + metal-rough only.
The feature bitset drives **dynamic uniform branches** in `evalSurface` (zero new PSOs) so a material
without a normal map pays nothing for the normal path.

## Design

- **Packed ORM** is first-class (decision: collapse 3 maps → 1 sample → 1 bindless slot). Channel
  routing is recorded in the `.smat` (`{"ao":"r","roughness":"g","metallic":"b"}`), defaulting to the
  ORM/glTF order (occlusion R, roughness G, metallic B). A standalone single-role map overrides a
  packed channel when both are present (importer resolves this into the final slot set in phase 08).
- **Feature bits** (in `MaterialParams.tex1.w`): `HAS_NORMAL`, `HAS_ORM`, `HAS_EMISSIVE_TEX`,
  `HAS_HEIGHT` (used phase 06). `evalSurface` branches on them.
- **UV transform**: `uv = input.uv0 * mp.uv.xy + mp.uv.zw`, applied once at the top of `evalSurface`
  (and reused by every sample).
- **Normal map**: sample `tex0.z`, decode `xy*2-1`, reconstruct z, scale xy by `normalStrength`,
  transform by the TBN from `worldNormal`+`worldTangent` (phase 04).

`evalSurface` (extending phase 01):

```slang
SurfaceData evalSurface(MaterialInput m)
{
    float2 uv = m.uv0 * m.uvTiling + m.uvOffset;
    SurfaceData s;
    float4 base = albedoTextures[NonUniformResourceIndex(m.tex0.x)].Sample(uv);
    s.albedo = base.rgb * m.baseColor.rgb;  s.opacity = base.a * m.baseColor.a;

    float ao = 1.0, rough = m.mrFactor.y, metal = m.mrFactor.x;
    if (m.features & HAS_ORM) { float4 orm = albedoTextures[NonUniformResourceIndex(m.tex0.y)].Sample(uv);
        ao = orm[m.ormAo]; rough *= orm[m.ormRough]; metal *= orm[m.ormMetal]; }
    s.metallic = saturate(metal); s.roughness = clamp(rough, 0.045, 1.0); s.occlusion = ao;

    float3 n = normalize(m.worldNormal);
    if (m.features & HAS_NORMAL) { float3 t = normalize(m.worldTangent.xyz);
        float3 b = cross(n, t) * m.worldTangent.w;
        float3 nt = albedoTextures[NonUniformResourceIndex(m.tex0.z)].Sample(uv).xyz * 2 - 1;
        nt.xy *= m.normalStrength; n = normalize(nt.x*t + nt.y*b + nt.z*n); }
    s.normal = n;

    s.emissive = m.emissiveIn;
    if (m.features & HAS_EMISSIVE_TEX) s.emissive *= albedoTextures[NonUniformResourceIndex(m.tex0.w)].Sample(uv).rgb;
    return s;
}
```

(Channel routing `ormAo/ormRough/ormMetal` are small uint indices packed into a params lane, or fixed
to RGB if you choose convention-only routing — simpler; revisit if a provider needs otherwise.)

## Files to touch

- `engine/source/saffron/rendering/renderer_types.cppm` — extend `SubmeshMaterial` (Ref<GpuTexture>
  `normal`, `orm`, `emissiveTex`; `normalStrength`; uv tiling/offset; `features`). `MaterialParamsData`
  already has the lanes (phase 02) — populate `tex0.zw`/`tex1`/`uv`/`pbr.z`.
- `engine/source/saffron/rendering/renderer_drawlist.cpp` — pack the new fields into `MaterialParamsData`
  in the dedup builder; default missing textures to the right neutral (white for albedo/orm, flat-normal
  `(0.5,0.5,1)` for normal, black for emissive).
- `engine/source/saffron/assets/assets.cppm` — the `lower` lambda in `resolveEntityMaterials` and the
  `MaterialAsset`→`SubmeshMaterial` resolve (phase 09 consumer) load the new textures + set `features`.
- `engine/assets/shaders/mesh.slang` — extend `MaterialInput`/`evalSurface` per above; feature-bit consts.
- A neutral **flat-normal** default texture alongside `defaultWhiteTexture` (for `HAS_NORMAL`-off safety).

## Steps

1. Extend `SubmeshMaterial` + the resolve/lower paths + the drawlist packer + the `.smat` `features`.
2. Add the flat-normal default texture to the renderer's defaults.
3. Extend `evalSurface` with UV transform, ORM, normal mapping, emissive tex behind feature bits.
4. Author a test `.smat` referencing a normal + ORM (use 8-bit PNGs so phase 07 isn't a prerequisite);
   eyeball the sphere/scene; lighting must respond to the normal map.

## Gate / done

- `make engine` clean; a material with a PNG normal map visibly shows surface relief under a moving light.
- Materials with **no** maps render identically to before (feature bits all off).
- `make prepare-for-commit` clean. Docs: extend the Materials page with the PBR slot set.

## Risks

- **Normal convention**: this phase assumes OpenGL (+Y) normals; phase 07 bakes DX→GL at import so the
  shader stays branch-free. Test with a known `_gl` map.
- **Bindless pressure**: up to 4 textures/material now; ORM packing is the mitigation, slot reclamation
  is phase 15. Fine for test scenes.
- Keep the no-feature path a pure branch (uniform, wave-coherent) — don't sample maps you won't use.
