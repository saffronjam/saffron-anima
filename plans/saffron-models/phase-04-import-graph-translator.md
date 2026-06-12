# Phase 04 — Import-graph translator + `ImportOptions`

**Status:** COMPLETED
**Depends on:** 03

## Goal

Refactor `importModelWithMaterial` (`geometry.cppm:1027`) into a **translator** that produces a
format-neutral in-memory **import graph** (the existing `ImportedModel`: `nodes`, `materials`, `skin`,
`animations`) with **deterministic stable sub-ids** derived from source node/material names — and
introduce a serializable `ImportOptions` struct that owns *all* import decisions (scale, axis, per-role
colorspace policy, tangent generation, embed-vs-reference textures). No on-disk format change yet:
existing `importModel` keeps working, now driven by the graph + options. Defers: the `.smodel` writer (05).

## Why

This is UE Interchange's **read → decide** split: the translator (cgltf/tinyobj) knows formats but no
engine policy; `ImportOptions` is the single place every decision lives. Two payoffs the plan needs:
(1) reimport must be a *pure function* of `(source bytes, options, importerVersion)` so `sourceHash`
gating (phase 13) works — that requires deterministic sub-ids and centralized options; (2) a future OBJ/FBX
translator becomes "another front-end feeding the same graph," not a parallel pipeline.

## Stable sub-ids (the determinism rule)

Today ids are minted ad-hoc (`newUuid`) and the hierarchy dies after spawn (`ImportResult`,
`assets.cppm:112`). Instead, each sub-asset gets an id derived from a **stable source key**, so a reimport
that reorders meshes still matches the same sub-asset (the Unity "identity, not enumeration-order" lesson):

```cpp
// stable across reimports: hash(sourceFile-relative key). NOT array index, NOT newUuid().
Uuid subIdFor(std::string_view modelKey, std::string_view kind, std::string_view sourceName, u32 dupIndex);
// e.g. subIdFor("town", "material", "stone", 0); collisions on duplicate names disambiguated by dupIndex.
```

The model's own id is minted once at first import and stored in the MetadataChunk; on reimport it is read
back, not regenerated.

## `ImportOptions`

```cpp
struct ImportOptions {
    f32  scale = 1.0f;
    enum class Axis { YUp, ZUp } axis = Axis::YUp;
    bool genTangents = true;          // derivative-based today; option records intent
    bool embedTextures = true;        // always true for v1 (bake into container); reserved for later
    // per-role colorspace policy (replaces the scattered srgb=true/false at registerTextureBytes sites):
    // albedo/emissive → Srgb; normal/orm/metallic/roughness/occlusion/height → Linear; .hdr → Hdr.
    Colorspace colorspaceFor(MaterialMapRole role) const;
    nlohmann::json toJson() const;    // stored verbatim in MetadataChunk.import.options
    static ImportOptions fromJson(const nlohmann::json&);
};
```

The colorspace decision currently lives implicitly at the `registerTextureBytes(srgb=…)` call sites; this
phase centralizes it into `colorspaceFor(role)` so the writer (05) and the scan/`.smeta` (10) share one
source of truth and a scanned texture's colorspace is recoverable.

## The translator shape

```cpp
// Pure: source file + options → graph. No GPU, no disk writes, no catalog mutation, no spawn.
std::expected<ImportedModel, std::string>
translateModel(const std::filesystem::path& source, const ImportOptions&);
```

`ImportedModel` keeps `nodes` (name, parent index, TRS), `materials` (`ImportedMaterial` w/ map roles +
raw texture bytes/keys), `skin` (`joints`, `inverseBind`, `skeletonRoot`, `meshNode`), `animations`.
Texture bytes stay behind keys/offsets where possible (lazy) so a 50-texture parse doesn't hold every
decoded image at once. Each node/material/mesh/texture is assigned its stable sub-id here.

## Files to touch

- `engine/source/saffron/geometry/geometry.cppm` — extract `translateModel` out of
  `importModelWithMaterial`; add `subIdFor`, `MaterialMapRole`, and keep `extractGltfMaterial` /
  glTF texture extraction (`geometry.cppm:411`) feeding the graph.
- `engine/source/saffron/assets/assets.cppm` — add `ImportOptions` (+ `colorspaceFor`); make `importModel`
  (`assets.cppm:2128`) call `translateModel(source, options)` then its existing bake-`.smesh`/register-
  textures path (unchanged output this phase, just sourced from the graph).

## Steps

1. Define `MaterialMapRole` (albedo, normal, orm/metallicRoughness, occlusion, emissive, height) and
   `ImportOptions` with `colorspaceFor`.
2. Carve `translateModel` out of `importModelWithMaterial`; assign stable sub-ids via `subIdFor`; keep the
   cgltf/tinyobj parsing intact.
3. Repoint `importModel` to `translateModel` + the existing write/register tail, so behavior is identical
   (same `.smesh`, same texture files) — proven by the existing import e2e still passing.
4. Add a **determinism self-test**: `translateModel` the same source twice → identical graphs (same
   sub-ids, same node order, same material refs); a float-stability check on positions/tangents.

## Gate / done

- `make engine` clean; the existing import e2e still passes (no output change); the determinism self-test
  passes (two translations are identical); `make prepare-for-commit` clean.

## Risks

- **Determinism is load-bearing and easy to break:** cgltf/tinyobj iteration order, texture extraction
  via buffer-view vs URI (`geometry.cppm:411`), and float precision must be stable or `sourceHash`
  comparisons (13) and the cache (11) churn. The self-test must catch this.
- **Sub-id collisions on duplicate source names:** two materials both named "Material" need the
  `dupIndex` disambiguation; define the tie-break deterministically (source declaration order).
- **Scope creep:** resist baking the container here — this phase must leave on-disk output unchanged so a
  regression is obviously the refactor, not the new format.
