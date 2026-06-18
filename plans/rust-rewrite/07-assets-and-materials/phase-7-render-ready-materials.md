# Phase 7 — render-ready materials

**Status:** COMPLETED

**Depends on:** 07-assets-and-materials:phase-4-resolve-and-load-paths, 07-assets-and-materials:phase-2-material-asset-and-serde, 03-ecs-and-scene (Material/MaterialSet/MaterialComponent + MaterialSlot)

## Goal

Port the resolution from a loaded `MaterialAsset` (or scene material components) to a render-ready
`SubmeshMaterial` (with bindless GPU texture handles): `build_submesh_material` (the generic
`Uuid -> Arc<GpuTexture>` loader closure), `resolve_material_asset` (the main-thread instantiation over
`load_texture_asset`), and `resolve_entity_materials` (the per-entity Material/MaterialSet/MaterialComponent
precedence + the codegen-shader override + the proxy albedo for DDGI). Plus the `ResolvedMaterials`
result type.

## Why this shape (NO LEGACY)

`build_submesh_material` takes the texture loader as a closure (`&dyn Fn(Uuid) -> Option<Arc<GpuTexture>>`)
so both the main draw path (passing `load_texture_asset`) and the thumbnail worker (passing its own
uploader) share one mapping — the C++ `std::function` becomes a borrowed closure, no allocation, no
trait object needed for a single-call-site abstraction. The packed ORM/ARM map feeds **both** the
metallic-roughness and occlusion slots (one map → roughness G, metalness B, AO R), and `alpha_clip` is
derived from `blend == "masked"`. `resolve_entity_materials` keeps the exact precedence:
`MaterialAssetComponent` (a `.smat` id, with the codegen `_mesh.spv` override when a non-foldable graph
is on disk) wins; else `MaterialSetComponent` (per-submesh slots, clamped to the slot count); else
`MaterialComponent` (a single inline material). The proxy albedo (the resolved base color's rgb) is
captured for the DDGI voxel proxy. This is where the asset crate's resolved material meets the render
path; it returns owned `SubmeshMaterial`s holding `Arc<GpuTexture>` clones from the cache.

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `buildSubmeshMaterial` (the `loadTex` closure, the ORM
  → both mr + occlusion slots, `alphaClip = (blend == "masked")`), `resolveMaterialAsset`
  (`buildSubmeshMaterial` with `loadTextureAsset`), `resolveEntityMaterials` (the
  MaterialAsset/MaterialSet/MaterialComponent precedence, the `_mesh.spv` codegen override probe via
  `loadMaterialAssetRaw` + `lowerGraphToParams`, `out.proxyAlbedo`/`out.unlit`/`out.shader`),
  `ResolvedMaterials` (`{submeshes, unlit, proxyAlbedo, shader}`).
- Upstream scene: `MaterialAssetComponent`, `MaterialSetComponent`, `MaterialComponent`, `MaterialSlot`,
  `SubmeshMaterial` (rendering), `has_component`/`get_component`.

## Acceptance gate

- `cargo build -p saffron-assets` + workspace green; clippy + fmt clean.
- `build_submesh_material` `#[test]`: a material with an ORM map populates both the metallic-roughness and
  occlusion texture handles from the same id; `alpha_clip` is true iff `blend == "masked"`; a zero
  texture id leaves the handle unset (the draw path's default-white substitution is a renderer concern).
- `resolve_entity_materials` `#[test]`s (over a stub scene + stub texture loader): the precedence order
  (MaterialAsset > MaterialSet > MaterialComponent); a MaterialSet entity produces one submesh material
  per mesh submesh clamped to the slot count; a missing `.smat` id falls back to the default material +
  warns; `proxy_albedo` reflects the resolved base color.
- The codegen-override `#[test]`: a MaterialAsset whose raw graph is non-foldable and whose `_mesh.spv`
  exists sets `out.shader` to that `.spv`; a foldable graph leaves the shared übershader.
