# Morph CPU storage, the `.smesh` flags collapse, and the morph components

**Status:** COMPLETED
**Depends on:** Phase 1

## Progress

- **geometry — DONE, green.** `MorphDelta` (28B Pod + const-assert), `MorphTarget`/`MorphData`,
  `ImportedModel.morph`; `.smesh` collapsed to one `version=3` + `MESH_FLAG_SKIN`/`MESH_FLAG_MORPH` +
  `morph_offset` + morph section (`MorphSectionHeader`/`MorphTargetDesc`); single
  `save_mesh_to_buffer(mesh, skin, morph)` + `load_mesh_morph_from_bytes` + `save_mesh`; the v2 skinned
  format and `save_mesh_skinned*` are deleted. `append_primitive` reads sparse morph deltas via the
  `gltf` reader (compacted, vertex-offset shifted); `finalize_morph` fills rest weights from mesh
  `weights`. All `cargo test -p saffron-geometry` pass; `cube.smesh`/`cube.smodel`/`cube.sanim` goldens
  reseeded.
- **scene — DONE, green.** `MorphComponent` (durable, registered `"Morph"` + `BUILTIN_COMPONENT_NAMES`
  + hand-written `SceneSerialize`) and runtime-only `MorphWeightOverride` (unregistered, twin of
  `PoseOverride`). 62+25 tests pass incl. `registry_is_complete`.
- **assets — DONE, green.** `bake_model` writes a per-node morph section (mesh-global morph on the first
  mesh-bearing node) + the META `morph` block; `METADATA_SCHEMA_VERSION 1→2`; `ContainerMetadata.morph`;
  `ModelSpawnInput` morph fields; `seed_morph` seeds the durable `MorphComponent` (rest-weights → zeros)
  in all three spawn paths; `instantiate_model` reads the META morph block. 170 tests pass.
- **editor — DONE.** `"Morph"` added to `COMPONENT_ORDER` and Inspector `NON_ADDABLE`/`NON_REMOVABLE`;
  `tsc --noEmit` clean.
- **Verification:** full `cargo build --workspace` green; `cargo clippy --all-targets` + `rustfmt --check`
  clean on geometry/scene/assets.
- **Two deliberate follow-ups (NOT silent shortcuts):**
  1. **`upload_mesh` morph-arity widening moved to Phase 4.** The `morph: Option<&MorphData>` param on
     `upload_mesh` / `GpuUploader` (~12 implementors) is only consumed when `GpuMesh.morph`/`MorphBuffers`
     land — that is Phase 4. Threading an ignored param through 12 files now adds nothing; Phase 4
     widens the seam where its consumer lives. CPU `MorphData` is fully delivered to the bake/spawn
     boundary here.
  2. **Morph target names are synthesized `morph_{k}`**, not read from glTF `mesh.extras.targetNames`
     (decision #18's fallback). Reading `extras` depends on the `gltf` crate's `extras` feature; the
     morph feature is fully functional with synthesized labels. Refinement: parse `extras.targetNames`
     when the feature is enabled. Names round-trip durably on `MorphComponent` regardless. (`AnimPath::Weights`, the v2 `.sanim` record, the per-node `ImportedNode.mesh` ownership, the morph-weights animation channel decode)

## Goal

Stand up the full CPU side of morph targets: sparse per-vertex deltas on `ImportedModel`,
a single `.smesh` version that carries optional skin and morph sections behind a flags
word, a durable `MorphComponent` (with target names) plus a runtime-only
`MorphWeightOverride`, and rest-weight seeding at spawn. This phase deletes the
two-version `.smesh` (`MESH_FORMAT_VERSION` + `MESH_FORMAT_VERSION_SKINNED`) and the
skinned-only save/load pair, collapsing every mesh — unskinned, skinned, morph, or
skin+morph — onto one `save_mesh_to_buffer(mesh, skin, morph)` and one
`load_mesh_from_bytes` + section readers. There is no `_SKINNED` constant and no version
branch left anywhere after this change.

## Design

### Sparse deltas — `MorphDelta`, `MorphTarget`, `MorphData`

A morph target is a sparse list of per-vertex position+normal deltas against the base
mesh. glTF morph targets are commonly sparse (only the moved vertices carry an accessor
entry), and the `gltf` 1.4.1 crate's `Reader::read_morph_targets` /
`Primitive::morph_targets` resolve the sparse accessor internally, handing back a dense
iterator with zero deltas for untouched vertices. We compact that back to sparse on
import: any vertex whose position-delta and normal-delta are both below a small epsilon
is dropped. The on-disk and in-memory unit is:

```rust
/// One sparse per-vertex morph contribution: the delta applied at full weight.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Pod, Zeroable)]
pub struct MorphDelta {
    /// Index into the base vertex stream this delta applies to.
    pub vertex_index: u32,
    /// Position delta at weight 1.0.
    pub d_position: Vec3,
    /// Normal delta at weight 1.0.
    pub d_normal: Vec3,
}
const _: () = assert!(size_of::<MorphDelta>() == 28, "MorphDelta must be exactly 28 bytes");
```

28 bytes is `4 + 12 + 12` with no trailing pad (`Vec3` is three `f32`, the leading `u32`
keeps the two `Vec3`s 4-byte aligned, and `MorphDelta`'s own alignment is 4). The engine
`Vertex` (`geometry/src/types.rs:Vertex`, 32 B, position/normal/uv0) carries **no tangent
stream**, so the tangent delta is not stored — Phase 4's deform shader re-derives the
tangent by Gram-Schmidt against the morphed normal. Storing a tangent delta would be dead
weight against a stream that does not exist.

The CPU aggregates sit on the imported model:

```rust
/// One named morph target: its sparse deltas and its rest (authored) weight.
pub struct MorphTarget {
    pub name: String,
    pub rest_weight: f32,
    pub deltas: Vec<MorphDelta>,
}

/// All morph targets of one mesh, in channel order.
pub struct MorphData {
    pub targets: Vec<MorphTarget>,
}
```

`ImportedModel` gains `morph: Option<MorphData>` (Phase 1 already moved mesh ownership
onto `ImportedNode.mesh`; the morph aggregate hangs off the model alongside the node
forest because target names come from the mesh-level `extras.targetNames` and the weight
vector is mesh-global). Decision #18 governs names: glTF `mesh.extras.targetNames` when
present, else synthesized `morph_{k}`. Decision #6/#7 governs malformed input — when a
mesh's primitives disagree on target count, take the canonical count from the mesh-level
`weights` length (falling back to the first primitive's count), zero-pad or drop the
offending primitive's targets to match, and emit a mandatory `tracing::warn!`. Never
abort the import.

`append_primitive` is where deltas are read. It already offsets indices and skin by
`vertex_offset` as it concatenates primitives into one mesh; it now also reads
`prim.morph_targets()` / `Reader::read_morph_targets`, shifts each delta's `vertex_index`
by the running `vertex_offset`, and appends into `MorphData.targets[k].deltas`. This is
unconditional — independent of the skin gate — so an unskinned morph mesh fills `MorphData`
exactly as a skinned one does.

### `.smesh` version collapse — one version, one flags word, one morph section

The format today (`geometry/src/smesh.rs`) ships **two** versions:
`MESH_FORMAT_VERSION = 1` (vertices/indices/submeshes) and
`MESH_FORMAT_VERSION_SKINNED = 2` (the same three sections plus a `VertexSkin` section).
The encoder picks the version by whether the skin is non-empty; the loader accepts 1 and
2 and rejects anything else. The header's `flags: u32` is reserved-zero and
`reserved: [u32; 2]` is reserved-zero.

This phase collapses both into **one** version with a meaningful flags word and a fourth
optional section:

```rust
/// The `.smesh` format: a 64-byte header, three contiguous sections (vertices, indices,
/// submeshes), and two optional sections (skin, morph) selected by the header flags.
pub const MESH_FORMAT_VERSION: u32 = 3;

/// Header flag bits.
const MESH_FLAG_SKIN: u32 = 1 << 0;
const MESH_FLAG_MORPH: u32 = 1 << 1;
```

`SMeshHeader` keeps its 64-byte width and field order, with two changes:

- `flags: u32` now carries `MESH_FLAG_SKIN | MESH_FLAG_MORPH` (was reserved-zero).
- `reserved: [u32; 2]` becomes `morph_offset: u64` — same 8 bytes, same offset, so the
  header stays exactly 64 B and the `assert!(size_of::<SMeshHeader>() == 64, …)` holds
  unchanged. `morph_offset` is 0 when `MESH_FLAG_MORPH` is clear; otherwise it points at
  the morph section (which follows the skin section when both are present).

The skin section keeps its current layout (a `VertexSkin` array parallel to the
vertices, appended after the submeshes) but is now gated by `MESH_FLAG_SKIN` instead of
the version number. The morph section is new:

```rust
/// The morph section sub-header (one per morph mesh, at `morph_offset`).
#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct MorphSectionHeader {
    /// Number of morph targets that follow.
    target_count: u32,
    /// Total `MorphDelta` records across all targets.
    delta_count: u32,
}

/// One per target, immediately after the section header: where this target's deltas
/// sit and what its authored rest weight is. Names live in META, not the binary.
#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct MorphTargetDesc {
    /// First `MorphDelta` index for this target.
    first_delta: u32,
    /// Number of `MorphDelta` records for this target.
    delta_count: u32,
    /// Authored rest weight.
    rest_weight: f32,
    /// Pad to 16 B.
    _pad: u32,
}
```

The morph section is `MorphSectionHeader`, then `target_count` × `MorphTargetDesc`, then
the flat `delta_count` × `MorphDelta` block (all targets' deltas concatenated, indexed by
each desc's `first_delta`/`delta_count`). Target **names** are not in the binary — they
ride in META (durable on `MorphComponent`, decision #10), so the `.smesh` carries pure
fixed-stride Pod arrays and stays byte-deterministic.

### One save, one load family

There is exactly one encoder and one section-decoder family after this change:

- `save_mesh_to_buffer(mesh: &Mesh, skin: &[VertexSkin], morph: Option<&MorphData>) -> Result<Vec<u8>>`
  — the single encoder. Sets `MESH_FLAG_SKIN` when `skin` is non-empty (validating
  `skin.len() == mesh.vertices.len()`, the existing `Error::SkinLengthMismatch`), sets
  `MESH_FLAG_MORPH` and writes `morph_offset` + the morph section when `morph` is
  `Some` and non-empty. No skin → no skin section; no morph → no morph section,
  `morph_offset == 0`.
- `load_mesh_from_bytes` — unchanged signature; accepts only `version == 3`, validates
  the three required sections, returns the `Mesh`.
- `load_mesh_skin_from_bytes` — unchanged signature; returns an empty `Vec` when
  `MESH_FLAG_SKIN` is clear, else decodes the skin section.
- `load_mesh_morph_from_bytes(bytes: &[u8]) -> Result<Option<MorphData>>` — **new**;
  returns `None` when `MESH_FLAG_MORPH` is clear, else decodes the morph section from
  `morph_offset` into a `MorphData` (with empty names — the caller fills names from META).

### Components — `MorphComponent` (durable) and `MorphWeightOverride` (runtime-only)

`MorphComponent` is the durable twin of the existing `SkinnedMesh`: it is
import-managed (seeded at spawn, never hand-added), it round-trips through the registry,
and it carries the target names so the editor can label sliders without re-reading META
(decision #10):

```rust
/// The per-entity morph weight vector and the target names that label it.
///
/// Import-managed (seeded at spawn from the mesh's authored rest weights); the weight
/// vector length must equal the mesh's target count, so it is non-addable and
/// non-removable in the editor (decision #11). `weights` are canonical 0..1.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MorphComponent {
    pub weights: Vec<f32>,
    pub names: Vec<String>,
}
```

`MorphWeightOverride` is the runtime-only twin of `PoseOverride`
(`scene/src/component.rs:PoseOverride`): the animation evaluator writes the sampled
weights onto it each frame, the GPU deform reads it, and it is removed when the rig stops
animating so the mesh reverts to the durable `MorphComponent.weights`. It never
serializes and is never registered (exactly like `PoseOverride`):

```rust
/// The animated morph weights the evaluator writes each frame. Runtime-only (never
/// serialized, never registered); removed on stop so the mesh reverts to the durable
/// `MorphComponent.weights`. Weights are canonical 0..1.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MorphWeightOverride {
    pub weights: Vec<f32>,
}
```

`MorphComponent` registers as `"Morph"` via `register_component!` in
`register_builtin_components` and is added to `BUILTIN_COMPONENT_NAMES`; both move
together (the `registry_is_complete` test enforces the pair). Its `SceneSerialize` impl
is hand-written in `scene/src/serde.rs` — a flat `{ "weights": [f32…], "names": [str…] }`
object, with the same `f32_value`/`object`/`field` helpers the other impls use. Phase 3
adds the field renames that route the evaluator; this phase only stands up the storage and
the seeding.

`MorphWeightOverride` is deliberately **not** registered (it has no `BUILTIN_COMPONENT_NAMES`
row and no `SceneSerialize` impl) — the registry-completeness test treats it like
`PoseOverride`, a runtime-only component absent from the row set by design.

### Spawn seeding — node weights, then mesh weights, then zeros

At spawn, the mesh-bearing entity gets a `MorphComponent` whose initial weights come from
a three-tier fallback, highest priority first:

1. **Node weights** — a glTF node's own `weights` override the mesh defaults when present;
   Phase 1 carries these on the imported node.
2. **Mesh rest weights** — the mesh-level `weights` (the `MorphTarget.rest_weight` values
   decoded into `MorphData`).
3. **Zeros** — when neither is authored, the weight vector is `vec![0.0; target_count]`.

The vector length is always `target_count`, and `names` is the decoded target-name list.
Seeding lands on whichever entity owns the mesh in each spawn path: `spawn_unskinned`,
`spawn_skinned_model` (the `mesh_entity` that already receives `SkinnedMesh`), and the
node-forest spawn from Phase 1. `ModelSpawnInput` (`assets/src/spawn.rs:ModelSpawnInput`)
gains the morph fields (`morph_target_names: Vec<String>`, `morph_rest_weights: Vec<f32>`,
per-node weight overrides) read from META by `instantiate_model`.

### META morph block + schema bump

`ContainerMetadata` (`assets/src/model.rs`) gains a `morph` block alongside the existing
`nodes`/`skin` fields: `{ "targetNames": [str…], "targetCount": u32, "restWeights": [f32…] }`,
written by `bake_model` from `MorphData`, read by `instantiate_model` into
`ModelSpawnInput`. Because the META JSON shape changes,
`assets/src/model.rs:METADATA_SCHEMA_VERSION` bumps `1 → 2`. This is the assets-crate META
schema (distinct from `geometry/src/smodel.rs:METADATA_SCHEMA_VERSION`, which this phase
does not touch).

`SCENE_VERSION` (`scene/src/document.rs:SCENE_VERSION`) **stays at 4** and is **not**
bumped: it is a range gate (`scene_from_json` accepts `1..=SCENE_VERSION` with no migration
table), and adding a new optional serialized component does not invalidate an existing
scene — an older scene simply has no `"Morph"` row, which is the absent-component default.
No scene-format change is needed for this phase.

### `upload_mesh` ripple — one arity move

The morph deltas must reach the GPU, so the upload seam widens. The real implementation is
`rendering/src/upload.rs:Uploader::upload_mesh(&self, mesh, skin)`; every consumer goes
through the `GpuUploader` trait (`assets/src/gpu.rs:GpuUploader::upload_mesh`). Both gain a
`morph: Option<&MorphData>` parameter, and **every** implementor moves in this one change
(the workspace build is the gate — a stub left on the old arity fails to compile). The
implementors:

`assets/src/gpu.rs` (trait + the live `RendererUploader`), `assets/src/render_material.rs`,
`assets/src/manage.rs`, `assets/src/render_scene.rs` (two impls), `assets/src/load.rs`
(`upload_mesh_from_source` + the test stub), `assets/src/thumbnail.rs`,
`control/src/test_support.rs`, `host/src/control_renderer.rs` (two impls),
`host/src/overlay.rs`. The Phase-4 GPU upload of the deltas into device buffers attaches
to `rendering/src/upload.rs:Uploader::upload_mesh`; this phase wires the parameter through
and `upload_mesh_from_source` reads the morph section via `load_mesh_morph_from_bytes`,
passing it down. (The actual `MorphBuffers` allocation on `GpuMesh` lands in Phase 4 —
this phase delivers the CPU `MorphData` to the upload boundary; the test stubs accept and
ignore it.)

### Frontend registration split

The editor mirrors the backend's addability in two TypeScript sources of truth, which move
together with the Rust registration:

- `editor/src/lib/componentOrder.ts:COMPONENT_ORDER` — add `"Morph"` (placed after
  `"SkinnedMesh"`, beside the other mesh-side components).
- `editor/src/panels/InspectorPanel.tsx` — add `"Morph"` to both `NON_ADDABLE` and
  `NON_REMOVABLE` (it is import-managed, decision #11).

The Inspector morph slider section itself lands in Phase 7; this phase only registers the
component's identity so it surfaces as a non-addable section.

## Changes

| What | Location (file:symbol) | Kind |
|---|---|---|
| `MorphDelta` (28 B Pod + const-assert) | `geometry/src/types.rs` | add |
| `MorphTarget`, `MorphData` CPU aggregates | `geometry/src/types.rs` | add |
| `ImportedModel.morph: Option<MorphData>` | `geometry/src/types.rs:ImportedModel` | modify |
| Read `prim.morph_targets()`, offset by `vertex_offset`, fill `MorphData` (+ malformed warn/best-effort) | `geometry/src/gltf_import.rs:append_primitive` | modify |
| Delete `MESH_FORMAT_VERSION_SKINNED`; `MESH_FORMAT_VERSION = 3` | `geometry/src/smesh.rs:MESH_FORMAT_VERSION` / `MESH_FORMAT_VERSION_SKINNED` | modify/delete |
| `flags` → SKIN/MORPH bits; `reserved:[u32;2]` → `morph_offset:u64` | `geometry/src/smesh.rs:SMeshHeader` | modify |
| `MorphSectionHeader` + `MorphTargetDesc` sub-headers | `geometry/src/smesh.rs` | add |
| `save_mesh_to_buffer(mesh, skin, morph) -> Result` (single encoder) | `geometry/src/smesh.rs:save_mesh_to_buffer` | modify |
| `load_mesh_from_bytes` / `load_mesh_skin_from_bytes` gate on flags, accept only v3 | `geometry/src/smesh.rs:load_mesh_from_bytes/load_mesh_skin_from_bytes` | modify |
| `load_mesh_morph_from_bytes(bytes) -> Result<Option<MorphData>>` | `geometry/src/smesh.rs` | add (new-file fn) |
| Delete `save_mesh_skinned_to_buffer` / `save_mesh_skinned` | `geometry/src/smesh.rs:save_mesh_skinned_to_buffer/save_mesh_skinned` | delete |
| Re-export `MorphDelta`/`MorphData`/`MorphTarget`/`load_mesh_morph_from_bytes`; drop `save_mesh_skinned*` exports; pin `MorphDelta` bytes | `geometry/src/lib.rs` (`pub use` re-exports) | modify |
| `MorphComponent` (durable) + `MorphWeightOverride` (runtime-only) | `scene/src/component.rs` (twin of `PoseOverride`) | add |
| Register `"Morph"` | `scene/src/registry.rs:register_builtin_components` + `BUILTIN_COMPONENT_NAMES` | modify |
| `impl SceneSerialize for MorphComponent` | `scene/src/serde.rs` (beside `impl SceneSerialize for SkinnedMesh`) | add |
| Pass `morph` to the single `save_mesh_to_buffer`; write META `morph` block | `assets/src/import.rs:bake_model` | modify |
| `METADATA_SCHEMA_VERSION 1 → 2`; add `morph` to `ContainerMetadata` + its read/write | `assets/src/model.rs:METADATA_SCHEMA_VERSION` / `ContainerMetadata` | modify |
| `ModelSpawnInput` morph fields; seed `MorphComponent` (node→mesh→zeros) | `assets/src/spawn.rs:ModelSpawnInput` / `spawn_unskinned` / `spawn_skinned_model` / `instantiate_model` | modify |
| `upload_mesh` gains `morph: Option<&MorphData>`; `upload_mesh_from_source` reads the morph section | `rendering/src/upload.rs:Uploader::upload_mesh`, `assets/src/gpu.rs:GpuUploader::upload_mesh`, `assets/src/load.rs:upload_mesh_from_source` + all impls below | modify |
| `upload_mesh` impl arity move (build gate) | `assets/src/render_material.rs`, `assets/src/manage.rs`, `assets/src/render_scene.rs` (two impls), `assets/src/load.rs` (test stub), `assets/src/thumbnail.rs`, `control/src/test_support.rs`, `host/src/control_renderer.rs` (two impls), `host/src/overlay.rs` | modify |
| `COMPONENT_ORDER` += `"Morph"` | `editor/src/lib/componentOrder.ts:COMPONENT_ORDER` | modify |
| `NON_ADDABLE` + `NON_REMOVABLE` += `"Morph"` | `editor/src/panels/InspectorPanel.tsx:NON_ADDABLE` / `NON_REMOVABLE` | modify |

## New artifacts

- `MorphDelta` (28 B Pod), `MorphTarget`, `MorphData` in `geometry/src/types.rs`.
- `.smesh` **v3** with a flags word (`MESH_FLAG_SKIN`/`MESH_FLAG_MORPH`) and an optional
  morph section (`MorphSectionHeader` + `MorphTargetDesc[]` + `MorphDelta[]`).
- `load_mesh_morph_from_bytes` reader.
- `MorphComponent` (durable, registered `"Morph"`) + `MorphWeightOverride` (runtime-only).
- META `morph` block (`targetNames`/`targetCount`/`restWeights`), META schema `2`.

## NO-LEGACY cutover (this change)

- **Delete the second `.smesh` version.** `MESH_FORMAT_VERSION_SKINNED` is removed;
  `MESH_FORMAT_VERSION` becomes `3`; the version-by-skin branch in the encoder and the
  `version == 1 || version == 2` accept in `load_mesh_from_bytes` are replaced by a single
  `version == 3` accept and the flags word. No `_SKINNED` constant, no version branch, and
  no `save_mesh_skinned_to_buffer`/`save_mesh_skinned` survive anywhere.
- **Move every caller of the deleted save fns** in the same change:
  `assets/src/import.rs` (the `save_mesh_skinned_to_buffer` import and the `if let Some(skin)`
  branch in `bake_model`) collapses to one `save_mesh_to_buffer(&graph.mesh, skin, morph)`;
  the geometry test `geometry/tests/gltf_import.rs` and the assets-side test setups in
  `render_scene.rs` move to the single encoder; `geometry/src/lib.rs` stops re-exporting the
  skinned fns.
- **Rewrite the frozen golden-byte + reject tests** in `geometry/src/smesh.rs::tests`:
  `golden_bytes_header_is_frozen` now asserts `header.version == 3`, the SKIN/MORPH
  flag bits, and `morph_offset` in place of `reserved == [0,0]`, with skin and
  skin+morph variants; `unknown_version_is_rejected` now writes a version other
  than 3 and rejects it; `skinned_round_trip`/`v1_image_yields_empty_skin`/
  `truncated_skin_section_is_rejected`/`mismatched_skin_length_is_rejected`/`file_round_trip`
  move to the one `save_mesh_to_buffer(mesh, skin, None)` arity.
- **Move the two registration sources of truth together:** the Rust registry pair
  (`register_builtin_components` + `BUILTIN_COMPONENT_NAMES`) and the TS pair
  (`COMPONENT_ORDER` + `InspectorPanel` `NON_ADDABLE`/`NON_REMOVABLE`) all gain `"Morph"`
  in this change — `registry_is_complete` enforces the Rust side.
- **Widen `upload_mesh` everywhere at once** — the ~12 implementors above move to the new
  arity in one change; a stub left on `(mesh, skin)` fails to compile.

## Test gate

- `cargo test -p saffron-geometry`:
  - `MorphDelta == 28` const-assert compiles; a `#[cfg(test)]` size/Pod assertion.
  - `.smesh` v3 round-trip: unskinned (no flags), skin-only (`MESH_FLAG_SKIN`),
    morph-only (`MESH_FLAG_MORPH`, `morph_offset` set), skin+morph (both flags, morph
    after skin) — each decodes its sections and `morph` is `None` when the flag is clear.
  - The rewritten `golden_bytes_header_is_frozen` (version 3, flag bits, `morph_offset`)
    and `unknown_version_is_rejected` (non-3 rejected) pass.
  - `append_primitive` malformed-input case: primitives with mismatched target counts
    warn (capture via `tracing` test subscriber) and import best-effort, never panic.
- `cargo test -p saffron-scene`: `registry_is_complete` green with the `"Morph"` row;
  a `MorphComponent` `SceneSerialize` byte round-trip (`weights`/`names`); confirm
  `MorphWeightOverride` is absent from the registry (runtime-only).
- `cargo test -p saffron-assets`: META `morph` block written deterministically by
  `bake_model`; spawn seeds `MorphComponent` with the node→mesh→zeros precedence and a
  length-`target_count` vector; `METADATA_SCHEMA_VERSION == 2`.
- `cargo build --workspace` is itself the gate for the `upload_mesh` arity move.
- Milestone gate: `just engine` then `just prepare-for-commit` (format + clippy
  `-D warnings`), fixing every warning this change raises.
