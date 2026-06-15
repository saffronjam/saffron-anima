# Saffron.Assets

The asset layer: the catalog + uuid-keyed GPU resource caches, the unified `project.json` format, the
native `.smat` material system (factors, textures, instances, node graphs), and `renderScene` — the
scene-to-renderer draw driver. Single-file module `Saffron.Assets` (`assets.cppm`), namespace `se`,
classic `#include` in the global module fragment, does **not** `import std`. `AssetCatalog`/`AssetEntry`/
`AssetType` themselves live in `Saffron.Scene`; this module owns the `AssetServer` that wraps them with
GPU caches. Imported by `Saffron.Control` (the asset/scene/animation command files) and `Saffron.Host`.

## Map (one file, by concern)

| Concern | Symbols |
|---|---|
| Catalog + GPU cache | `AssetServer`, `meshRefByUuid`/`textureRefByUuid`, `clearAssetCaches`, catalog↔json |
| Project I/O | `saveProject`/`loadProject`/`createProject`, `ProjectVersion`, app-data/user roots |
| Import | `importModel`/`bakeModel`/`instantiateModel`/`reimportModel`, `importTexture`/`registerTextureBytes`, `loadMeshAsset`/`loadTextureAsset`/`loadAnimationClipAsset` |
| Materials | `.smat` serde, `loadMaterialAsset`, `applyOverrides`, `lowerGraphToParams`, `compileMaterial*Shader`, `resolveEntityMaterials` |
| Scene driver | `renderScene`, `pickEntity`, `spawnModel`/`spawnSkinnedModel` |

## Rules that are easy to break

- **A cached `null` `Ref` is a negative-cache marker, not a miss.** A failed mesh/texture is stored as a
  null `Ref` so it is *not* retried (or re-logged) every frame; a dangling texture id falls back to the
  default white in the draw path. Don't treat an empty slot as "not loaded yet".
- **Clear caches only after `waitGpuIdle`.** `loadProject`/`createProject` call `waitGpuIdle(renderer)`
  *then* `clearAssetCaches` before swapping the catalog, so in-flight `Ref`s are never freed under the
  GPU. Never clear without the idle.
- **`renderScene` is the orchestrator, not a helper — and it lives here**, not in `Saffron.Rendering`.
  It updates world transforms, fits the directional shadow frustum to the scene, picks the shadowed
  spot/point light, drives DDGI/sky/env, and splits the ray-tracing TLAS (static instances carry their
  model matrix; skinned meshes ride the draw list as identity because their deformed verts are already
  world-space). The skinned path is gated on skinning being enabled.
- **Material instances are parent + sparse overrides.** `parent != 0` resolves to the parent's params
  with this material's `overrides` applied on top (edit-once-propagate: editing a parent reflows every
  instance); `0` is a master material. `DefaultMaterialId{1}` short-circuits to `defaultMaterialAsset()`.
- **Node-graph materials are folded when they can be.** `lowerGraphToParams` collapses a constant/
  texture-only graph into flat params; any procedural/math node forces Slang codegen. The mesh-shader
  path splices the generated surface body into the runtime `mesh.slang` between the `// @graph-begin` /
  `// @graph-end` markers and passes `-I <shaders dir>` so `import lighting` resolves. `findSlangc`
  resolves `slangc` from `SAFFRON_SLANGC`, then the slang cache, then `PATH`.
- **Texture colorspace is set at import.** Albedo/emissive upload as sRGB; data maps (normal,
  metallic-roughness, occlusion, height) upload linear (`linear = !srgb`).
- **`project.json` is version-gated** (`ProjectVersion`, a mismatch is an error) and bundles assets +
  folders + scene + render settings + optional `editorCamera` and `debugOverlays` blocks, which are
  round-tripped back to the caller because the camera and overlays belong to `Saffron.SceneEdit`, not here.

A change here that adds drivable/inspectable state still needs a `registerCommand` in `Saffron.Control`
(most asset commands live in `control_commands_asset.cpp`) and a `docs/` update — see the root `AGENTS.md`.
