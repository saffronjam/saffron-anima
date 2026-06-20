# Phase 9 — spawn and instantiate

**Status:** COMPLETED

**Depends on:** 07-assets-and-materials:phase-8-import-bake-and-scan, 03-ecs-and-scene (the ECS world + component inserts + hierarchy), 04-animation (skin/animation components present)

## Goal

Port the scene-spawning side: `model_spawn_input` reconstruction from a container's META
(`instantiate_model`), `spawn_model` / `spawn_skinned_model` (place a `ModelSpawnInput`'s mesh +
material table — and for a rigged glTF the node forest + bone entities + skin descriptor — as scene
entities holding soft references), and the META node/skin block decode (`imported_nodes_from_json`,
`imported_skin_from_json`). `ModelSpawnInput` is the input these consume; it is never an import output
(reconstructed from META, not produced by `bake_model`).

## Why this shape (NO LEGACY)

This is the one phase that mutates the scene ECS, so it takes `&mut Scene`. Spawned components hold
**soft references** — sub-ids resolved at draw time by phase-4's loaders, not live `Arc` handles — so a
spawned entity serializes cleanly into `project.json` and re-resolves on load. `spawn_model` dispatches
to `spawn_skinned_model` when `has_skin`. The skinned path spawns the node forest as child entities, the
bone entities, and the `SkinnedMeshComponent` with the skin descriptor; the root carries a
`ModelInstanceComponent` so the editor treats the placed model as a unit. The C++ stores the quaternion
in glTF `w,x,y,z` order in the META; the Rust decode reorders to glam's `xyzw` at the byte boundary
(this reorder lives in geometry/scene per the foundations glam rule — this phase consumes already-glam
TRS). `instantiate_model` is the public entry; `spawn_*` are the lower-level builders.

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `instantiateModel` (META → `ModelSpawnInput` → spawn,
  tags the root `ModelInstanceComponent`), `spawnModel` (dispatch on `hasSkin`), `spawnSkinnedModel`
  (node forest + bone entities + skin), `ModelSpawnInput` (`{mesh, baseColor, albedoTexture, materials,
  hasSkin, nodes, skinDesc, animations}`), `importedNodesFromJson`, `importedSkinFromJson` (the META
  node/skin block decode), `ensureBuiltinModelAsset`.
- Upstream scene: the ECS world spawn/insert API, `IdComponent`, `TransformComponent`,
  `MeshComponent`/`SkinnedMeshComponent`, `MaterialSetComponent`, `ModelInstanceComponent`, the hierarchy
  (parent/child) helpers; animation: skin/animation-player components.

## Acceptance gate

- `cargo build -p saffron-assets` + workspace green; clippy + fmt clean.
- An integration `#[test]`: `bake_model` a flat (unrigged) cube fixture → `instantiate_model` spawns one
  entity with a `MeshComponent` (the mesh sub-id), a base color, and — when >1 material — a
  `MaterialSetComponent` with the slots in slot order; the spawned mesh id matches the baked sub-id.
- A skinned `#[test]`: instantiating a rigged glTF fixture spawns the node forest + bone entities + a
  `SkinnedMeshComponent` with the skin descriptor and the root tagged `ModelInstanceComponent`; the
  registered animation ids are attached.
- A determinism `#[test]`: instantiating the same container twice yields entities referencing the same
  sub-ids (soft references stable across instantiations); the META node/skin quaternion decode reorders
  to glam `xyzw` (assert a known node's rotation).

## Post-integration fix (e2e exposed)

Wiring instantiate through the e2e (`materials_render`/`normal_render`/`material_update_texture`)
exposed that a spawned model's `Material` carried only the flat factors (base color / metallic /
roughness) and **never the imported texture sub-ids** — so an imported glTF's albedo / MR / normal map
read `0` and the shaded pixels never changed. The spawn material loop read the META `materials` summary
(factors only); the texture sub-ids live in the container's `SMAT` material chunk. Fixed:
`instantiate_model` now resolves each material sub-asset's full `.smat` from its container chunk
(`resolve_container_material` → `material_asset_from_json`) and copies every texture slot + tiling/
strength/alpha field into the `MaterialSlot` — the C++ `resolveMaterial` per slot. A chunk that fails
to resolve falls back to the META flat factors (logged, never fatal). The texture sub-ids resolve
through the catalog to bound bindless slots at draw time, so the imported textures now reach the GPU.
