# CPU substrate: generalized track model, node forest, import-gate lift, format and mesh-ownership cutover

**Status:** COMPLETED
**Depends on:** — (nothing)

## Goal

Make the CPU import + format substrate carry the full feature, not just skeletal. Every glTF —
skinned, unskinned-multinode, OBJ, or static — decodes a **node forest** plus one **heterogeneous
clip** of bone-, node-, and morph-weight tracks, in one unconditional decode. There is exactly one
mesh-ownership shape (mesh lives on a forest node), one widened `AnimTrack` carrying a `target` and a
`morph_count`, one `AnimPath` that includes `Weights`, and one `.sanim` v2 record. The morph-weight
*decode* and the sampler *shape* land here; the runtime routing of node and weight tracks to their
write seams lands in Phase 3 and the morph delta payload lands in Phase 2, but the track model and the
format that hold them are complete in this phase.

This phase folds blueprint subsystems 1 (generalized track model), 2 (node forest), and 3 (import-gate
lift). The fold is forced by the data: the gate-lift produces the forest **and** the clips in a single
`import_gltf_model` decode; the track model's `target_name` **is** the forest node name it binds to;
and the gate-lift cannot exist without both the forest (to know which nodes are animated) and the
widened track (to carry node vs bone targets). Splitting them would strand a half-built forest behind a
skin-only clip path — a NO-LEGACY violation in flight.

## Design

### One track model

`AnimTrack` today is bone-shaped (`joint: i32`, `joint_name: String`) and `AnimPath` is T/R/S only. The
generalized model adds a second axis — *what kind of thing the track drives* — without a three-way
target enum (decision #17):

- New `enum AnimTarget { Bone = 0, Node = 1 }` (`#[repr(u8)]`, pinned discriminants, `from_u8`
  rejecting out-of-range with `Error::BadLayout`, mirroring `AnimPath`/`AnimInterp`). A morph-weight
  track is `AnimTarget::Node` + `AnimPath::Weights` — it binds to a forest node by name and drives that
  node's mesh weights. There is no `AnimTarget::MorphWeight` arm.
- `AnimPath` gains `Weights = 3`; `from_u8` extends to map `3`.
- `AnimTrack` field renames make the bone-specific names generic, since the field now addresses bones,
  nodes, and morph sets:
  - `joint: i32` → `index: i32` (resolved bone index for `Bone`; `-1` and unused for `Node`/`Weights`,
    which bind by name).
  - `joint_name: String` → `target_name: String` (the durable binding key — a node name for every
    target kind).
  - add `target: AnimTarget`.
  - add `morph_count: u32` (the number of weights per keyframe for a `Weights` track; `0` for T/R/S).
    `values.len() == times.len() * morph_count * stride`, where `stride` is `1` for Step/Linear and `3`
    for CubicSpline.

`AnimClip` keeps its shape (`name`, `duration`, `tracks: Vec<AnimTrack>`) but now holds heterogeneous
tracks.

### One node forest, one mesh-ownership path (decision #4)

`build_skin` already builds an `ImportedNode` forest, but only behind the skin gate and only stored on
`SkinPayload`. The forest is lifted to a free function and to `ImportedModel`:

- New free fn `build_node_forest(document, parents) -> Vec<ImportedNode>` (the node loop extracted
  verbatim from `build_skin` at `gltf_import.rs:build_skin`, including the `Matrix`/`Decomposed`
  branch). `build_skin` calls it instead of inlining the loop.
- `ImportedModel` gains `nodes: Vec<ImportedNode>` and `animations: Vec<AnimClip>`. Clips are no longer
  parked on `SkinPayload` — they are a top-level property of the model, decoded for skinned and
  unskinned alike.
- `ImportedNode` gains `mesh: Option<Mesh>` — the node-local merged mesh for the primitives under that
  glTF node. This is the *single* mesh-ownership shape (decision #2: airtight `Option`, no
  parallel-index `Vec`).
- **`ImportedModel.mesh` is DELETED** (decision #4). OBJ import and the single-skinned-mesh case route
  their geometry through a node carrying `mesh: Some(...)` — there is no top-level mesh field for
  anything to read. One ownership shape, one code path. `SkinPayload` loses its `nodes` and
  `animations` fields (they move to `ImportedModel`); it keeps `stream` (per-vertex skin influences)
  and `desc` (the `ImportedSkin`).

### Import-gate lift

The skin gate at `gltf_import.rs:import_gltf_model` currently branches three ways (`skins_count == 0`
with mesh-nodes → world-baked flatten; `skins_count == 0` without → mesh-flatten; `skins_count > 0` →
skinned). The lifted shape is uniform:

- Read `has_animations = document.animations().next().is_some()` up front.
- Always run `build_node_forest` and `decode_clips` (clips bind by node name regardless of skin).
- A mesh-bearing node gets its primitives appended into **that node's** `mesh: Option<Mesh>` with
  `node_transform = None` — the geometry stays in node-local space, parented through the forest's
  `parent` indices. The per-node world-transform vertex bake is **deleted**: a node animated by a
  node-TRS track must keep its drivable local transform, so baking the world transform into vertices
  (which discards that transform) is exactly the path node-TRS animation cannot tolerate.
- `build_parents` stays (the forest needs parent indices); `world_transform` is deleted along with its
  only caller (the unskinned bake).

### Single-node collapse (decision #3, strict identity-only)

Collapse to a single entity happens *only* when the forest is one root node with an identity local
transform. A single non-identity or animated node keeps a container entity above the mesh-bearing
entity, because folding its TRS into the mesh entity's `Transform` would erase the local transform a
node-TRS track drives. This rule is enforced in `assets/src/spawn.rs` (Phase 1's spawn changes); the
import always produces the full forest, and spawn decides whether it collapses. (The cube fixture is a
single identity root → it still collapses to one entity with a node-local mesh.)

### decode_clips generalization

`decode_clips` currently skips three channel classes — it is rebuilt to route every channel:

- Delete the `Property::MorphTargetWeights` skip (`decode_clips` morph-weights guard): a morph-weights
  channel now decodes into a `Weights` track. Read the N-wide weights via
  `ReadOutputs::MorphTargetWeights`, flattening `times.len() * morph_count` (×3 for CubicSpline) floats.
  `morph_count` is the per-keyframe weight count — it comes from the **target mesh's weight-vector
  length** (the mesh-level `weights` length, the canonical morph-target count established in Phase 2's
  import; falling back to the first primitive's target count). For a `Weights` channel the track is
  `target: Node`, `path: Weights`, `index: -1`, `target_name` = the channel's target node name.
- Delete the non-skin-node skip (`decode_clips` non-skin guard): a channel targeting a node that is not
  in the skin's joint list is no longer dropped — it decodes as `target: Node` with `index: -1` and
  binds by name. A channel whose target node **is** a skin joint decodes as `target: Bone`,
  `index = joint position`, exactly as today.
- Delete the sparse-sampler skip (`decode_clips` sparse guard): the `gltf` 1.4.1 `Reader` resolves
  sparse accessors internally through `read_inputs`/`read_outputs`, so the defensive sparse rejection is
  dead — sparse samplers decode like dense ones. The `(None, None)` empty-reader guard stays (a genuinely
  empty sampler is still skipped with a warning).
- `decode_clips` no longer takes `desc: &ImportedSkin` for *gating* — it takes the joint list to decide
  Bone vs Node, and the forest `nodes` for the `target_name` lookup. It runs unconditionally from
  `import_gltf_model` (skinned or not), not from inside `build_skin`.

### Malformed morph input at import (decisions #6 / #7)

Cross-primitive morph-target-count disagreement (a glTF spec violation) is handled **at import, warn +
best-effort, never abort**. The canonical target count is the mesh-level `weights` length (fallback: the
first primitive's count). A primitive that disagrees is zero-padded up or dropped down to the canonical
count with a **mandatory** `tracing::warn!` (silent best-effort is forbidden). This count is also the
`morph_count` a `Weights` track carries. The actual delta extraction lands in Phase 2's
`append_primitive` change; Phase 1 fixes the *count source* (mesh weights length) so the `Weights`
track decode and the Phase 2 delta decode agree on one number.

### Sampler shape (the substrate is complete here; Phase 3 owns the write-seam routing)

`sample_clip_resolved` (`animation/src/runtime.rs:sample_clip_resolved`) reads `track.joint` and
`track.joint_name`; the renames break it. In this phase it is updated to the new field names and gated
on `track.target == AnimTarget::Bone`: Bone tracks rebind and write `PoseBuffer.local` exactly as
today; Node and Weights tracks are recognized by the model but pass through untouched here. The full
node-`PoseOverride` and morph-`MorphWeightOverride` routing — and the N-wide `sample_weights` sampler —
land in Phase 3. The *track model and format that carry these tracks are complete now*; only the
runtime write seam for the two new target kinds is wired in Phase 3. No part of the feature's data is
absent from the substrate.

### `.sanim` v1 → v2 (decision #5)

The 20-byte `SANimTrackRecord` cannot carry `target` or `morph_count`. The record widens to **24 bytes**
and `ANIM_FORMAT_VERSION` bumps **1 → 2**; v1 is rejected with `Error::UnsupportedVersion(1)`. The new
record:

```rust
#[repr(C)]
struct SANimTrackRecord {
    index: i32,        // renamed from `joint`
    target: u8,        // AnimTarget discriminant
    path: u8,          // AnimPath discriminant
    interp: u8,        // AnimInterp discriminant
    pad: u8,           // explicit, always 0
    morph_count: u32,  // weights-per-key for a Weights track, else 0
    name_len: u32,     // bytes of target_name following the record
    time_count: u32,
    value_count: u32,
}
// const _: () = assert!(size_of::<SANimTrackRecord>() == 24, ...);
```

`save_animation_to_buffer` / `load_animation_from_bytes` write/read the four discriminant-derived
fields and `morph_count`; the version guard accepts only `2`. The golden fixture `cube.sanim` is
reseeded under `UPDATE_GOLDEN=1` in the same change (the 20 → 24B record shifts every byte after the
first record's start, and the record's field layout changes outright).

## Changes

| What | Location (file:symbol) | Kind |
|---|---|---|
| `AnimTarget { Bone = 0, Node = 1 }` + `from_u8` | `engine/crates/geometry/src/types.rs` (new enum near `AnimPath`) | new |
| `AnimPath::Weights = 3`; extend `from_u8` | `engine/crates/geometry/src/types.rs:AnimPath` | modify |
| `AnimTrack`: `joint`→`index`, `joint_name`→`target_name`, +`target: AnimTarget`, +`morph_count: u32` | `engine/crates/geometry/src/types.rs:AnimTrack` | modify |
| `ImportedNode.mesh: Option<Mesh>` | `engine/crates/geometry/src/types.rs:ImportedNode` | modify |
| `ImportedModel`: +`nodes: Vec<ImportedNode>`, +`animations: Vec<AnimClip>`; **remove** `mesh` | `engine/crates/geometry/src/types.rs:ImportedModel` | modify/delete-field |
| `SkinPayload`: **remove** `nodes`, `animations` (keep `stream`, `desc`) | `engine/crates/geometry/src/types.rs:SkinPayload` | modify/delete-field |
| `build_node_forest(document, parents) -> Vec<ImportedNode>` (lifted from the `build_skin` node loop) | `engine/crates/geometry/src/gltf_import.rs:build_node_forest` | new |
| `import_gltf_model`: read `has_animations`; always build forest + decode clips; per-node-local mesh, no world bake | `engine/crates/geometry/src/gltf_import.rs:import_gltf_model` | modify |
| `world_transform` (unskinned vertex bake) | `engine/crates/geometry/src/gltf_import.rs:world_transform` | delete |
| `append_primitive`: append into the node's local `Mesh` with `node_transform = None` | `engine/crates/geometry/src/gltf_import.rs:append_primitive` | modify |
| `build_skin`: drop the node loop (call `build_node_forest`), stop decoding clips, return only `stream`+`desc` | `engine/crates/geometry/src/gltf_import.rs:build_skin` | modify |
| `decode_clips`: delete the three skips; route Bone vs Node by joint-list membership; decode `Weights` N-wide; bind `target_name` from the forest | `engine/crates/geometry/src/gltf_import.rs:decode_clips` | modify |
| `to_track_path`: map `Property::MorphTargetWeights` → `AnimPath::Weights` | `engine/crates/geometry/src/gltf_import.rs:to_track_path` | modify |
| OBJ import routes its mesh through an `ImportedNode { mesh: Some(...) }` (no top-level mesh) | `engine/crates/geometry/src/obj_import.rs:import_obj_model` | modify |
| `ANIM_FORMAT_VERSION` 1 → 2; reject v1 | `engine/crates/geometry/src/sanim.rs:ANIM_FORMAT_VERSION`, version guard in `load_animation_from_bytes` | modify |
| `SANimTrackRecord` 20B → 24B (`index`, `target`, `path`, `interp`, `pad`, `morph_count`, three counts); const-assert 24 | `engine/crates/geometry/src/sanim.rs:SANimTrackRecord` | modify |
| `save_animation_to_buffer` / `load_animation_from_bytes`: write/read the new fields | `engine/crates/geometry/src/sanim.rs` | modify |
| Pin `AnimPath::Weights == 3`, `AnimTarget` discriminant bytes; re-export `AnimTarget` | `engine/crates/geometry/src/lib.rs` (the `assert_eq!` discriminant test + `pub use types::{...}`) | modify |
| `sample_clip_resolved`: rename `joint`/`joint_name`; gate the bone rebind+write on `AnimTarget::Bone`, pass through Node/Weights | `engine/crates/animation/src/runtime.rs:sample_clip_resolved` | modify |
| `nodes_for_graph`: read `graph.nodes` directly (not `graph.skin.nodes`); non-empty for unskinned | `engine/crates/assets/src/import.rs:nodes_for_graph` | modify |
| `bake_model`: read `graph.animations` (not `graph.skin.animations`); per-node `Mesh` sub-asset with a `"mesh"` META field; mesh-save reads the node's `Some(mesh)` | `engine/crates/assets/src/import.rs:bake_model` | modify |
| `imported_nodes_to_json`: emit the per-node `"mesh"` sub-id field | `engine/crates/assets/src/import.rs:imported_nodes_to_json` | modify |
| `spawn_node_forest` (instantiate the forest with node-local meshes) | `engine/crates/assets/src/spawn.rs:spawn_node_forest` | new |
| `spawn_model`: dispatch skin → skinned, forest → node-forest, single-identity → collapse; `imported_nodes_from_json` parses `"mesh"` | `engine/crates/assets/src/spawn.rs:spawn_model`, `imported_nodes_from_json` | modify |
| `cube_clip` fixture: new `AnimTrack` field names + `target`/`morph_count` | `engine/crates/geometry/tests/golden_snapshot.rs:cube_clip` | modify |
| Reseed golden `cube.sanim` | `engine/crates/geometry/tests/fixtures/cube.sanim` | modify |
| `sanim.rs` unit tests: 24B record, v1 reject, recomputed byte offsets | `engine/crates/geometry/src/sanim.rs:tests` | modify |
| `runtime.rs` test fixtures: new `AnimTrack` field names | `engine/crates/animation/src/runtime.rs:tests` | modify |

## New artifacts

- `AnimTarget` enum (geometry) + its `from_u8` and pinned discriminant assert.
- `build_node_forest` free fn (geometry).
- `spawn_node_forest` (assets) and the per-node `"mesh"` META field on each node entry.
- `.sanim` **v2** format (24-byte record carrying `target` + `morph_count`).
- Fixtures (added in Phase 7 but the *shape* is fixed here): a multi-node unskinned animated glTF
  (`BoxAnimated`-style) and a morph-weights-channel glTF — both decode through the unconditional forest
  + clip path.

## NO-LEGACY cutover (this change)

Superseded paths/fields **deleted in this change**, with every caller and test moved alongside:

- **`ImportedModel.mesh`** — removed entirely. All mesh ownership routes through `ImportedNode.mesh`.
  Every reader (`assets/src/import.rs:bake_model` mesh-save, OBJ import, any test constructing
  `ImportedModel`) moves to per-node meshes in the same change.
- **`SkinPayload.nodes` / `SkinPayload.animations`** — removed; the forest and clips live on
  `ImportedModel`. `build_skin` no longer decodes clips or builds the node loop; `nodes_for_graph` and
  `bake_model` read `graph.nodes` / `graph.animations`.
- **The unskinned world-transform vertex bake** — `world_transform` and its call site deleted; node
  geometry stays node-local.
- **The three `decode_clips` skips** — the morph-weights guard, the non-skin-node skip, and the sparse
  guard are deleted; channels route instead.
- **`AnimTrack.joint` / `AnimTrack.joint_name`** — renamed to `index` / `target_name`; every reader
  (`runtime.rs:sample_clip_resolved`, the runtime test fixtures, `golden_snapshot.rs:cube_clip`,
  `geometry/tests/gltf_import.rs`) updated.
- **`.sanim` v1** — rejected; `ANIM_FORMAT_VERSION == 2` is the only accepted value. The 20-byte record
  is gone. `golden_snapshot.rs:cube_sanim_bytes_match_cpp_golden` reseeds `fixtures/cube.sanim` under
  `UPDATE_GOLDEN=1`; `sanim.rs::tests::unsupported_version_is_rejected` now rejects v1 (and a v3 probe),
  and every test asserting a `36 + 20`-based byte offset is recomputed to `36 + 24`.

No old code path survives next to the new one; a `cargo clippy -- -D warnings` build is the strongest test
that no renamed field reader was missed.

## Progress

- **geometry crate — DONE, green.** `types.rs` (AnimTarget, AnimPath::Weights, AnimTrack
  rename+fields, ImportedNode.mesh, ImportedModel.nodes/animations, ImportedModel.mesh removed,
  SkinPayload trimmed), `gltf_import.rs` (node forest, gate lift, node-local meshes, decode_clips
  routing Bone/Node/Weights, world-bake deleted), `sanim.rs` (v2, 24B record), `obj_import.rs` (single
  node), `lib.rs` (discriminant asserts + re-export). All `cargo test -p saffron-geometry` pass; golden
  `fixtures/golden/cube.sanim` reseeded.
- **animation crate — DONE, green.** `sample.rs` + `runtime.rs` `sample_clip_resolved` gated on
  `AnimTarget::Bone`, field renames, Weights arm; test fixtures updated. All 33 tests pass.
- **NEXT: assets crate.** `import.rs` — bake per-node mesh sub-assets (one Mesh chunk per mesh-bearing
  node), `nodes_for_graph` reads `graph.nodes` with a per-node `"mesh"` sub-id field, `bake_model` reads
  `graph.animations`, `imported_nodes_to_json` emits `"mesh"`. `spawn.rs` — `spawn_node_forest`,
  `spawn_model` dispatch (skin → skinned; multi-node forest → node-forest; single identity root →
  collapse), `imported_nodes_from_json` parses `"mesh"`, `ModelSpawnInput` carries per-node mesh ids.
  Then `import_tests.rs`/`spawn_tests.rs` fixtures. The skinned path's mesh is the `desc.mesh_node`'s
  node mesh. Workspace ripple confirmed confined to geometry+animation+assets (rendering/host/control
  consume baked bytes + scene components, not `ImportedModel`).
- **assets crate — DONE, green.** `import.rs` (per-node mesh sub-assets keyed by node name,
  `imported_nodes_to_json` emits a per-node `"mesh"` sub-id, clips from `graph.animations`,
  `nodes_for_graph` deleted), `spawn.rs` (`spawn_node_forest`, `spawn_model` dispatch with
  single-identity collapse, `node_mesh_ids_from_json`, `ModelSpawnInput.node_meshes`, instantiate
  selects the mesh per skinned/forest shape), `load.rs` (single-mesh upload paths use
  `ImportedModel::primary_mesh()`). All 170 assets tests pass; fixtures in `import_tests.rs`/
  `spawn_tests.rs`/`manage.rs`/`scan_tests.rs` rebuilt on the node-forest shape.
- **Verification:** `cargo test` green for `saffron-geometry`, `saffron-animation`, `saffron-assets`;
  `cargo clippy --all-targets` clean and `rustfmt --check` clean on every changed file.
- **Full-workspace gate blocked by an environment issue, NOT this change:**
  `cargo build --workspace` fails in `saffron-physics-sys`'s C++ build script, which points its Jolt
  shim at `/var/home/saffronjam/repos/SaffronEngine/engine/crates/physics-sys/shim/jolt_bridge.cpp`
  (a different repo checkout than this `saffron-anima` tree) — `jolt_bridge.h`/`.cpp` not found there.
  Physics was not touched here; my changed crates do not depend on physics, so they were gated in
  isolation per AGENTS.md. The full `just engine`/`just prepare-for-commit` and the e2e/host build need
  this resolved.
- **Concurrency note:** the `assets-connectors` agent is actively editing this tree (uncommitted changes
  to `protocol/src/{command,codegen,dto}.rs`, `control/src/commands_asset.rs`, `editor/*`, regenerated
  `schemas/`). Phase 6 (control plane) will edit those same protocol files — coordinate the count rebase
  + `xtask gen-protocol` regen when it lands (see the README Coordination section).

## Test gate

- `cargo test -p saffron-geometry`:
  - `.sanim` v2 round-trips every field including `target` and `morph_count`; the reseeded golden
    `cube.sanim` matches; v1 and v3 are rejected with `UnsupportedVersion`; truncation/`BadLayout`
    guards hold at the recomputed offsets; `const_assert!(size_of::<SANimTrackRecord>() == 24)`.
  - `AnimPath::Weights as u8 == 3`, `AnimTarget::{Bone,Node} as u8 == {0,1}`, both `from_u8` reject
    out-of-range.
  - A multi-node unskinned animated import: `model.nodes.len() > 1`, a mesh-bearing node carries
    `mesh: Some(...)` with node-local (un-baked) vertices, parent indices match the source, and
    `model.animations` carries a `Node`-targeted clip.
  - The cube import still collapses to a single identity root with a node-local mesh.
  - A morph-weights-channel import yields a `Weights` track with the right `morph_count` and
    `times.len() * morph_count` (×3 CubicSpline) value floats.
- `cargo test -p saffron-assets`: META `nodes` is non-empty for the unskinned forest and carries a
  `"mesh"` field per mesh-bearing node; `bake_model` reads `graph.animations`; the single-identity
  collapse spawns one entity.
- `cargo test -p saffron-animation`: `sample_clip_resolved` Bone tracks still rebind and write
  `PoseBuffer.local`; Node/Weights tracks pass through without touching the pose (full routing arrives
  in Phase 3).
- Milestone gate (per AGENTS.md): `just engine` then `just prepare-for-commit` (format + clippy
  `-D warnings`), fixing every warning this change raises.
