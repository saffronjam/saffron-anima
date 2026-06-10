# Phase 2 — glTF clip import → `.sanim` + `AssetType::Animation`

**Status:** COMPLETED

## Goal

Stop dropping animation on import. Walk the glTF `data->animations[]` array (today completely ignored),
decode every channel into the `AnimClip`/`AnimTrack` types from Phase 1, write each clip to a sidecar
**`.sanim`** file (magic `SANM`), register one **`AssetType::Animation`** catalog entry per clip bound to
the skeleton by joint **name + index**, and spawn an `AnimationPlayerComponent` (Phase 3 defines its
fields; this phase just attaches it) on the imported rig. After this phase a rigged glTF with animations
round-trips: import → `.sanim` on disk → catalog entries → reload.

## What exists to build on

- `importGltfModel` (`geometry.cppm:383`) parses meshes, skins, and the node forest but **never touches
  `data->animations`** (confirmed absent, `:383-620`). It frees `cgltf` at the end of the skin block.
- cgltf accessor readers already used here: `cgltf_accessor_read_float` (`:472,476,482,492`),
  `cgltf_accessor_read_uint` (`:490`), `cgltf_accessor_read_index` (`:503`). The bind-matrix import already
  swaps glTF `[x,y,z,w]` quats for `glm::quat`'s `(w,x,y,z)` (`:566`) — reuse that convention.
- `ImportedModel` (`geometry.cppm:111`): `mesh`, `materials`, `hasSkin`, `skin`, `nodes`, `skinDesc`.
  `ImportedSkin` (`:79`): `joints` (node indices, the sacred order), `inverseBind`, `skeletonRoot`,
  `meshNode`. `ImportedNode` (`:67`): `name`, `parent`, TRS.
- `.smesh` IO precedent: `saveMeshSkinned(mesh, skin, path)` (`:164`), `loadMeshSkin(path)` (`:167`),
  `SMeshHeader` (`:186`, 64 B, magic `SMSH`, versioned). The `.sanim` writer mirrors this shape with its
  own magic so it never collides with `.smesh`.
- `AssetType` enum (`scene.cppm:210`) = `{ Mesh, Texture, Other }`. `AssetEntry` (`:217`) = `id`, `name`,
  `type`, `path`, `folder`, `hdr`, `linear`. `AssetCatalog` (`:228`) = `entries`, `folders`, `byId`.
- Catalog serde: `catalogToJson` (`assets.cppm:204`) / `catalogFromJson` (`:246`) are the **only**
  persistence points for assets — anything not written there is lost on save.
- `importModel` / `ImportResult` (`assets.cppm:50`) carry the import into the scene; `spawnSkinnedModel`
  (`assets.cppm:992`) makes the bone entities + `SkinnedMeshComponent` and calls `relinkHierarchy`.

## Work

### 1. Walk `data->animations[]` in `importGltfModel`

Inside the skin block (after the skin/node forest is built, before `cgltf_free`), iterate
`data->animations[0..animations_count)`. For each `cgltf_animation`:
- New `AnimClip{ name = anim.name ?: "clip_<i>" }`.
- For each `cgltf_animation_channel`:
  - Map `channel.target_node` → joint index by **matching the node pointer to the joint node list**
    (`skinDesc.joints` holds node indices; resolve the channel's target node to its index in
    `data->nodes`, then find that index's position in `skinDesc.joints`). Record both the resolved index
    and `node.name` on the track. Channels targeting a node **not** in the skin's joint set are skipped
    in v1 (logged) — only skinned-joint animation is in scope.
  - `channel.target_path` → `AnimTrack::Path` (`translation`/`rotation`/`scale`; **`weights` (morph) is
    skipped** in v1, logged).
  - `channel.sampler->interpolation` → `Interp` (`STEP`/`LINEAR`/`CUBIC_SPLINE`).
  - Read `sampler->input` (times) and `sampler->output` (values) with `cgltf_accessor_read_float` into the
    flat `times`/`values` vectors (stride: 3 for T/S, 4 for R, ×3 for CubicSpline). Guard
    `accessor.is_sparse` (cgltf returns 0s silently otherwise) — log + skip a sparse channel in v1.
  - `clip.duration = max(clip.duration, times.back())`.
- Append to a new `ImportedModel.animations` field (`std::vector<AnimClip>`; add it at `geometry.cppm:111`).

### 2. `.sanim` writer / loader in `Saffron.Geometry`

Add next to `saveMeshSkinned`:

```cpp
auto saveAnimation(const AnimClip& clip, const std::string& path) -> Result<void>;
auto loadAnimation(const std::string& path) -> Result<AnimClip>;
```

A 32-B header `SANimHeader { char magic[4]="SANM"; u32 version=1; u32 trackCount; f32 duration; u32 nameLen; u32 reserved[2]; }`
followed by the name bytes, then per-track: `{ i32 joint; u8 path; u8 interp; u32 nameLen; u32 timeCount;
u32 valueCount; }` + name bytes + `times` floats + `values` floats. Little-endian raw, like `.smesh`.
Keep it dead simple and versioned.

### 3. `AssetType::Animation` + catalog plumbing

- Add `Animation` to the `AssetType` enum (`scene.cppm:210`) → `{ Mesh, Texture, Other, Animation }`.
- `catalogToJson`/`catalogFromJson` (`assets.cppm:204`/`246`) already iterate arbitrary entries — extend
  the per-entry type-string map to include `"animation"`. Optionally serialize a `duration` field on the
  entry for animation rows (cheap, lets the timeline footer + `list-clips` report duration without loading
  the `.sanim`). If you add a field to `AssetEntry`, it must be written/read in both functions or it is lost.
- Pre-release migration: old `project.json` files predate `Animation`; loading an unknown type-string
  should fall back to `Other` rather than error (one line in `catalogFromJson`).

### 4. Register clips on import + attach the player

- Extend `ImportResult` (`assets.cppm:50`) with `std::vector<Uuid> animations` (the registered clip ids)
  and a `Uuid` per clip is minted as each `.sanim` is baked + catalogued.
- In `importModel`: after the skinned mesh is baked, for each `ImportedModel.animations[i]`, call
  `saveAnimation` to `<assetsDir>/<name>.sanim`, mint a Uuid, push an `AssetEntry{ type=Animation,
  path, name, folder }` into the catalog.
- In `spawnSkinnedModel` (`assets.cppm:992`): if `result.animations` is non-empty, add an
  `AnimationPlayerComponent` (Phase 3 type) to the **mesh entity** (the one carrying
  `SkinnedMeshComponent`), defaulting `clip` to the first imported clip, `playing=false`, `loop=true`.
  This is what makes a freshly-imported rig immediately playable in Phase 3.

## Validation (done criteria)

- `make engine` + `make prepare-for-commit` clean.
- A rigged+animated glTF imports: `.sanim` files appear next to the `.smesh`; `project.json` lists
  `AssetType::Animation` entries; reload reconstructs the same `AnimClip` (`loadAnimation` round-trip
  asserted in the geometry self-test or e2e).
- **Fixture:** add a small rigged+animated glTF to `engine/assets/models/` (e.g. a 2-bone bend or a
  cube-with-an-arm) — this is the fixture Phases 3 and 5 exercise. If a suitable asset isn't on hand,
  author a minimal one (a glTF with one skin, ≤4 joints, one LINEAR rotation clip is enough). Record its
  path in the Phase 5 e2e.
- `docs/`: update `gltf-and-obj-import.md` (it currently notes the lack of animation support) and the
  `smesh-format.md` neighbours with a `.sanim` format page.

## Notes / gotchas

- The **joint-order is sacred**: tracks bind to indices into `SkinnedMeshComponent.bones`, which is built
  from `skinDesc.joints`. Bind by both index (fast) and name (durable) — Phase 3's evaluator re-resolves
  by name if the index is stale after a reimport.
- A glTF may declare **multiple skins**; the importer already takes only the first (`skinDesc`). Clips
  whose channels target a different skin's joints are skipped in v1 — log it; don't silently drop.
- Do not bloat `SMeshHeader` or change `.smesh` — clips are strictly sidecar (the locked decision).
