# GPU morph deform: fixed-point atomic-scatter into the deformed buffer

**Status:** COMPLETED
**Depends on:** Phase 2 (sparse `MorphDelta` sections on the `.smesh`, `GpuMesh` carries them), Phase 3 (per-frame morph weights from the runtime evaluator).

## Progress

- **`engine/assets/shaders/morph.slang` — DONE, compiles to SPIR-V** (`xtask shaders`: "1 compiled").
  Three passes selected by `push.pass` (0 clear / 1 scatter / 2 resolve); `ByteAddressBuffer` scalar
  packing matching the 32 B `Vertex` + 28 B `MorphDelta` strides; bindings 0–5 (inVertices, inDeltas,
  targetRanges `uint2`, activeTargets `{targetIndex, scatterBase, weight}`, accum `6×i32`, outVertices);
  `Push {vertexCount, scatterCount, activeCount, deformedOffset, pass}`. Scatter does integer
  `accum.InterlockedAdd` of quantized `weight·delta` (deterministic), with a per-thread linear scan over
  `activeCount` to map a flat scatter-thread to its `(activeTarget, delta)` via `scatterBase`. Resolve
  dequantizes (`/MorphFixedScale`, 65536.0), adds base, renormalizes the normal. The Gram-Schmidt tangent
  is moot here — `Vertex` has no tangent stream — so resolve writes position/normal/uv0 only.
- **DONE — GPU buffer plumbing (builds green):**
  - `resources.rs`: `GpuMesh.morph: Option<MorphBuffers>` (+ `MorphBuffers {deltas, ranges, target_count,
    delta_count}`, Drop, `morph()` getter); `GpuMeshParts.morph`.
  - `upload.rs`: `Uploader::upload_mesh` widened to `(mesh, skin, morph: Option<&MorphData>)` + the
    self-contained `upload_morph_buffers` helper (flatten deltas + per-target `[first,count]` ranges →
    two STORAGE device buffers). The `GpuUploader` trait + **all ~12 impls + ~30 call sites** moved to the
    new arity (the deferred Phase-2 ripple); `upload_mesh_from_source` reads `load_mesh_morph_from_bytes`
    and passes the morph through. Full `cargo build --workspace` green.
  - `pipelines.rs`: `Pipelines.morph` + `request_morph` (builds `morph.spv` against the morph set layout,
    20-byte push).
- **DONE — draw-list cutover + Deform owner machinery (rendering lib compiles):**
  - `draw_list.rs`: `DrawBatch.skinned`→`deformed` (rename rippled through `scene_pass`/`aa`/`instancing`
    `match` arms + tests); `DrawItem` +`morph_weight_offset`/`morph_count` + `new()`; `MorphDispatch`;
    `SceneDrawList.morph_dispatches`/`prev_morph_dispatches` + `shallow_clone`.
  - `skinning.rs`: the `Deform` owner — `morph_set_layout` (6 storage buffers, created/destroyed),
    per-frame `accum` scratch field, `MORPH_FIXED_SCALE`/`ACCUM_STRIDE`, `MorphPush` Pod,
    `create_morph_set_layout`, `make_accum_buffer`, `wire_morph_set` (6 bindings), `record_morph`
    (clear→scatter→resolve with a `morph_accum_barrier` compute→compute barrier between passes/instances),
    `request_morph_pipeline`. Pool descriptor count bumped to `*6` for morph sets.
  - `cargo build -p saffron-rendering` succeeds; the only warnings are unused `accum`/`make_accum_buffer`
    (the morph buffer management is invoked by the instancing wiring in the next step).
- **DONE — wiring API + clippy-clean.** `skinning.rs`: `ensure_accum_capacity` + the
  `wire_morph_dispatches(frame, accum_vertices, deformed, active, meshes, dispatches)` method (mirrors
  `wire_dispatches`, fills each `MorphDispatch.set` via `wire_morph_set`). The public deform API
  (`record_morph`, `wire_morph_set`, `request_morph_pipeline`, `MORPH_FIXED_SCALE`, `MorphDispatch`) is
  re-exported from `rendering/src/lib.rs`. `cargo clippy -p saffron-rendering` is **clean**; full
  `cargo build --workspace` green. The machinery is complete and lint-clean but **not yet invoked at
  runtime** (steps below activate it).
- **DONE — runtime activation (full workspace build + clippy clean):**
  - `instancing.rs`: the deforms bucket (skin OR active morph); CPU active-target compaction
    (`MORPH_WEIGHT_THRESHOLD`, `ActiveTarget {target_index, scatter_base, weight}`) + the per-frame
    `active_targets` buffer; `SceneDrawList.morph_dispatches` populated from `Bucket.morph_weights`; the
    morph wiring call carries the `skin_ran` flag. `DrawItem.morph_weights: Vec<f32>` replaces the
    offset/count pair; `render_scene.rs::morph_weights_for` feeds both static + skinned `DrawItem`
    builders (`MorphWeightOverride` else `MorphComponent`).
  - `renderer.rs`: `FramePipelines.morph` resolved beside `skin`; a `morph` `RgPass`
    (`StorageWriteCompute` on the deformed buffer) recorded **before** skin via `crate::skinning::record_morph`,
    gated on `do_morph`. The deformed-buffer import is generalized to `do_deform = do_skin || do_morph`
    so a morph-only mesh imports + binds the deformed stream (geometry passes already declare
    `VertexInputRead`). morph→skin ordering is the graph's write-after-write on the shared deformed
    buffer (both passes declare `StorageWriteCompute`); no hand-written barrier. The morph push range
    comment in `skinning.rs` corrected to 24 bytes.
- **DONE — golden math test (`skinning::tests::morph_fixed_point_scatter_is_order_independent_and_matches_golden`):**
  a CPU mirror of `morph.slang`'s quantize→scatter→resolve that proves (a) integer-atomic scatter is
  order-independent (bit-identical forward vs. reverse), and (b) the dequantized blend reproduces the
  analytic `base + Σ wᵢ·δᵢ` within `3·0.5/MORPH_FIXED_SCALE`. The genuine on-GPU validation-clean run of
  `morph.spv` on llvmpipe is the Phase-7 e2e fixture (a morph entity drawn for N frames, asserting a
  validation-clean log) — the language-appropriate place per AGENTS.md.
- **v1 scope note:** a *skinned-morph* mesh (blend shapes on a skinned mesh, e.g. facial animation on a
  skinned head) has the skin pass overwrite the deformed slice from the bind pose, so morph is applied to
  morph-only meshes in v1. Chaining the skin pass to read the morphed base (binding 0 = the deformed
  buffer for morph-skinned instances) is the structural follow-up; the buffer contract already supports
  it (morph writes deformed first), only the skin set's input binding needs the variant.

## Goal

Run a `morph` compute pass that writes `base + Σ wᵢ·δposᵢ` (and a renormalized normal, Gram-Schmidt
tangent) into the shared deformed-vertex buffer **before** the skin pass, so skinning reads the morphed
base and the buffer chain — not a hand-ordered flag — enforces morph-before-skin. The kernel is the full
UE-shaped two-pass fixed-point atomic-scatter (decision #9): Pass A scatters `weight·delta` into per-vertex
integer accumulators via `atomicAdd` (integer atomics commute → bit-deterministic, required for golden
buffers and the llvmpipe CI GPU); Pass B dequantizes, adds the base vertex, renormalizes, and writes the
deformed stream. Each frame the CPU compacts active targets (drops below-threshold weights, UE
`GMorphTargetWeightThreshold` style). An unskinned-morph mesh has Pass B write the deformed buffer directly
and draws it as a static stream — one deformed-buffer contract for skin and morph alike.

## Design

### One owner: `Skinning` → `Deform`

The morph half and the skin half share the same deformed/prev-deformed buffers, the same grow policy, and
the same per-frame descriptor pool. There is no parallel `Morphing` struct — `rendering/src/skinning.rs`'s
`Skinning` is rewritten into a `Deform` owner with two compute halves over one set of buffers. The skin half
keeps `wire_dispatches` / `record_skin` / `swap_palette` / `commit_model` unchanged in behaviour; the morph
half adds the scatter scratch buffer, the morph descriptor set, and `record_morph`. The deformed buffer is
the single shared output: morph writes it (or the skin half reads it as its input then overwrites in place),
so the only ordering dependency is the data dependency on that buffer slice. The pool stays sized
`SKIN_MAX_SETS_PER_FRAME * 2` (`skinning.rs:SKIN_POOL_SET_CAPACITY`): the `* 2` is the cur + prev factor, and
a deformed instance allocates one descriptor set per half — a skin set on the skinned path, a morph set on
the morph path — out of that same cur/prev budget rather than both at once, so no capacity bump is needed.
A skin+morph instance reuses the same set slot across halves within a frame; `create_skin_pool` keeps its
descriptor-count derivation in step with the unchanged `* 2` capacity.

### `morph.slang` — two passes, fixed-point integer scatter

New shader `engine/assets/shaders/morph.slang` (auto-discovered by `xtask shaders`; same `ByteAddressBuffer`
scalar-packing discipline as `skin.slang` so the engine's tight 28 B `MorphDelta` and 32 B `Vertex` strides
match without a scalar-block-layout flag). Bindings, all compute-stage storage buffers:

- `(0)` `ByteAddressBuffer inVertices` — the static bind-pose stream (32 B `Vertex`).
- `(1)` `ByteAddressBuffer inDeltas` — the mesh's flat `MorphDelta` array (28 B: `u32 vertex_index`,
  `vec3 d_position`, `vec3 d_normal`).
- `(2)` `StructuredBuffer<MorphTargetRange>` — per-target `{u32 first_delta, u32 delta_count}` ranges into
  `inDeltas`.
- `(3)` `StructuredBuffer<ActiveTarget>` — the per-frame compacted active list: `{u32 target_index, f32 weight}`.
- `(4)` `RWByteAddressBuffer accum` — the per-vertex fixed-point scratch (6 × `i32` per vertex: position
  xyz + normal xyz), zeroed at the start of the dispatch.
- `(5)` `RWByteAddressBuffer outVertices` — the deformed output (32 B `Vertex`).

The push constant `Push { u32 vertexCount; u32 activeCount; u32 deformedOffset; u32 pass; }` selects the
pass and carries the instance's deformed-buffer base.

- **Pass `clear`** (one thread per vertex): `accum[i].* = 0` for the 6 lanes — a fixed-point zero so Pass A
  accumulates from a clean slate without a host clear submit.
- **Pass A — scatter** (one thread per active `(target, delta)`, flattened over `Σ activeTargets ×
  range.delta_count`): each thread reads its `ActiveTarget.weight` and its `MorphDelta`, computes
  `weight · d_position` / `weight · d_normal`, quantizes each component to `i32` via a fixed scale
  (`MorphFixedScale`, a `const` shared between shader and the Rust push side), and `atomicAdd`s the six
  integers into `accum[delta.vertex_index]`. Integer `atomicAdd` commutes, so the accumulated sum is
  independent of thread order — the result is bit-identical run to run and across the llvmpipe and hardware
  ICDs (required for the golden-buffer gate). No float atomics anywhere.
- **Pass B — resolve** (one thread per vertex): dequantize `accum[i]` (`i32 → f32 / MorphFixedScale`), add
  the base `inVertices[i]` position/normal, `normalize` the morphed normal, re-derive the tangent by
  Gram-Schmidt against the morphed normal (the engine `Vertex` carries no tangent stream — `geometry/src/types.rs:Vertex`
  is 32 B position/normal/uv0 — so a tangent delta would be dead weight), copy uv0 through, and `Store`
  the 32 B `Vertex` into `outVertices[(deformedOffset + i)]`. For an unskinned-morph mesh this is the final
  vertex stream the scene/depth/shadow passes read; for a skinned-morph mesh this is the `morphedBase` the
  skin half consumes.

`MorphFixedScale` is chosen so the largest plausible `Σ weight·delta` magnitude stays inside `i32` headroom
at sub-millimetre precision; it is a single `const` defined once and asserted in a Rust `cfg(test)` unit
test against the round-trip error bound the golden test uses.

### CPU active-target compaction (UE threshold-skip)

Each frame, for every morph instance the runtime produced weights for, the draw-list batcher builds the
compacted `ActiveTarget` list: iterate the instance's `MorphWeightOverride` (or durable `MorphComponent`)
weights, drop any with `|weight| < MORPH_WEIGHT_THRESHOLD` (a `const` mirroring UE's
`GMorphTargetWeightThreshold`), and emit `{target_index, weight}` for the survivors. A morph instance with
an empty active list after compaction skips the morph dispatch entirely and binds its static stream — the
deformed buffer is never touched for a rest-pose morph mesh, so it costs nothing. The active list and the
per-target ranges upload into a per-frame buffer (sized to the frame's total active count), bound at set
bindings 2/3.

### Skin reads the morphed base — the graph derives the barrier

The morph pass declares `StorageWriteCompute` on the deformed buffer; the skin pass declares
**`StorageReadCompute`** on the same deformed buffer (it now reads the morphed base as its input, not just
writes its output). `apply_access` (`render_graph.rs:apply_access`) computes its hazard as
`(target.is_write && r.touched) || (!target.is_write && r.last_was_write)`, so a `StorageReadCompute` after a
`StorageWriteCompute` on the same resource derives exactly one COMPUTE-WRITE → COMPUTE-READ memory barrier —
the morph→skin hand-off. This corrects the inverted framing in the existing skin pass comment
(`renderer.rs`, the `do_skin` block) that talks about a "compute-write → vertex-input barrier": that framing
is for the *geometry* consumers downstream, which keep `VertexInputRead` because they fetch the deformed
buffer as a vertex stream. The morph→skin edge is compute→compute and must be `StorageReadCompute`; it is
**not** `VertexInputRead` (wrong stage — the skin compute shader never reaches `VERTEX_ATTRIBUTE_INPUT`) and
it is **not** a hand-written barrier. The morph pass is added immediately before the skin pass in
`renderer.rs`'s render-graph build, gated by the same do-we-have-work predicate shape as `do_skin`.

For a skinned-morph mesh: morph Pass B writes the deformed slice → skin reads that slice as `inVertices`
input and overwrites it in place → geometry passes read the final slice. For an unskinned-morph mesh: morph
Pass B writes the deformed slice → no skin dispatch for that instance → geometry passes read it directly as
a static stream. Both are the one deformed-buffer contract.

### Draw-list plumbing

`DrawBatch.skinned` is renamed to `deformed` (decision #13) — its meaning is "binds the deformed buffer as
binding 0", true for a skinned OR a morph-active batch. Every reader moves in the same change (see the
cutover). `DrawItem` gains `morph_weight_offset: u32` / `morph_count: u32` locating the instance's compacted
weights in the per-frame morph-weights buffer; `DrawItem` has no `Default`, so the three `DrawItem { … }`
literals in `assets/src/render_scene.rs`, the literal in `rendering/src/instancing.rs`, and the test
literal in `rendering/src/pipelines.rs` all break and are updated in the same change (this is intended,
not a regression). `GpuMesh` gains `morph: Option<MorphBuffers>`
(`resources.rs:GpuMesh`) holding the delta buffer + per-target ranges uploaded in Phase 2; the morph half
wires its set from it, mirroring how the skin half wires from `mesh.skin_buffer()`.

A new `MorphDispatch` (twin of `SkinDispatch` in `draw_list.rs`) carries the per-instance morph descriptor
set, vertex count, active count, and deformed offset; `SceneDrawList` gains `morph_dispatches` and
`prev_morph_dispatches` (the prev-pose morph dispatch is wired in Phase 5 but the fields land here so the
shape is complete). `instancing.rs`'s `submit_draw_list_skinned` is renamed to `submit_draw_list_deformed`
and its signature widens to take `morph_weights: &[f32]` alongside `joints`; its caller in
`renderer.rs` (`submit_draw_list_skinned` → `submit_draw_list_deformed`) and the convenience forwarder
`renderer.rs:submit_draw_list` move with it. The "skinned" bucket selection in `submit_draw_list` becomes a
"deforms" bucket: an item buckets into its own deformed slice when it is skinned OR has a nonzero compacted
active-target list.

### New pipeline

`pipelines.rs:request_morph` (twin of `request_skin` at `pipelines.rs:request_skin`) builds the `morph`
compute PSO from `shaders/morph.spv` against the morph set layout owned by the `Deform` owner, caches it in
a new `morph: Option<Arc<Pipeline>>` field beside `skin`, and returns `None` on a build failure (logged).
`skinning.rs:request_skin_pipeline` gains a sibling `request_morph_pipeline`.

## Changes

| What | Location (file:symbol) | Kind |
|---|---|---|
| `morph` two-pass fixed-point scatter kernel | `engine/assets/shaders/morph.slang` | new-file |
| `Skinning` → `Deform` owner (morph half + skin half, shared buffers + pool) | `rendering/src/skinning.rs:Skinning` | modify |
| Morph set layout + per-frame scatter scratch + morph pool widening | `rendering/src/skinning.rs:create_skin_set_layout` / `create_skin_pool` | modify |
| `create_skin_pool` descriptor count covers the morph half within the unchanged `SKIN_POOL_SET_CAPACITY = * 2` (cur + prev) budget | `rendering/src/skinning.rs:create_skin_pool` | modify |
| `record_morph` (replay morph dispatches: clear → scatter → resolve) | `rendering/src/skinning.rs` | new |
| `MorphPush` repr(C) Pod (vertexCount/activeCount/deformedOffset/pass) | `rendering/src/skinning.rs` | new |
| `request_morph` PSO + `morph` cache field | `rendering/src/pipelines.rs:request_morph`, `Pipelines.morph` | new |
| `request_morph_pipeline` forwarder | `rendering/src/skinning.rs:request_morph_pipeline` | new |
| `DrawBatch.skinned` → `deformed` (binds deformed buffer) | `rendering/src/draw_list.rs:DrawBatch` | modify |
| `DrawItem` gains `morph_weight_offset` / `morph_count` (no `Default`) | `rendering/src/draw_list.rs:DrawItem` | modify |
| `MorphDispatch`; `SceneDrawList.morph_dispatches` / `prev_morph_dispatches` | `rendering/src/draw_list.rs:SceneDrawList` | new / modify |
| `GpuMesh.morph: Option<MorphBuffers>`; `MorphBuffers` handle struct | `rendering/src/resources.rs:GpuMesh` | modify / new |
| `submit_draw_list_skinned` → `submit_draw_list_deformed(view_proj, items, joints, morph_weights)` | `rendering/src/instancing.rs:submit_draw_list` (skinned variant) | modify |
| "skinned" bucket → "deforms" bucket (skin OR nonzero active list) + per-frame morph-weights buffer | `rendering/src/instancing.rs:submit_draw_list` | modify |
| Active-target compaction (threshold-skip) + `MorphDispatch` wiring | `rendering/src/instancing.rs` | new |
| `MORPH_WEIGHT_THRESHOLD`, `MorphFixedScale`, `MorphTargetRange`, `ActiveTarget` consts/Pod | `rendering/src/skinning.rs` / `instancing.rs` | new |
| Morph graph pass (`StorageWriteCompute` on deformed) before the skin pass | `rendering/src/renderer.rs` (the `do_skin` block) | new |
| Skin pass declares `StorageReadCompute` on deformed (morph→skin barrier) | `rendering/src/renderer.rs` (the `skin` `RgPass`) | modify |
| `bind_batch_vertices` keys on `batch.deformed` | `rendering/src/scene_pass.rs:bind_batch_vertices` | modify |
| `DrawItem` literals updated for new fields + `skinned`→bucket rename ripple | `assets/src/render_scene.rs` (three `DrawItem` literals) | modify |
| `DrawItem`/`DrawBatch` literals in instancing + test fixtures | `rendering/src/instancing.rs`, `rendering/src/pipelines.rs` (test literal) | modify |
| `submit_draw_list` forwarder + skinned submit renamed/rerouted | `rendering/src/renderer.rs:submit_draw_list` / `submit_draw_list_skinned` | modify |

## New artifacts

- `engine/assets/shaders/morph.slang` (→ `morph.spv` via `xtask shaders`).
- `request_morph` PSO + the `morph` cache field on `Pipelines`.
- `MorphDispatch`, `MorphBuffers`, `MorphTargetRange`, `ActiveTarget`, `MorphPush` GPU/host types.
- The per-frame morph-weights buffer and the per-frame active-target / range buffers on the deform path.
- The fixed-point scatter scratch buffer (6 × `i32` per vertex) owned by the `Deform` owner.

## NO-LEGACY cutover (same change)

- **`Skinning` is rewritten into `Deform`, not duplicated.** There is no `Morphing` struct beside it and no
  second deformed buffer — the morph half and skin half share the one buffer set and the one pool. The
  `SKIN_POOL_SET_CAPACITY = SKIN_MAX_SETS_PER_FRAME * 2` (cur + prev) budget is unchanged: the morph half
  draws its descriptor sets from the same cur/prev budget the skin half uses, so Phase 5's prev-morph
  dispatch needs no further capacity bump.
- **`DrawBatch.skinned` is deleted and replaced by `deformed`** (decision #13). Every reader moves in this
  change: `scene_pass.rs:bind_batch_vertices` (the `match (batch.skinned, deformed)` arm), the four other
  `bind_batch_vertices` call sites in `scene_pass.rs`, the bucket-selection in `instancing.rs:submit_draw_list`,
  and `aa.rs:record_motion`'s `match (batch.skinned, deformed, prev_deformed)` (Phase 5 finishes the prev
  side; the field rename lands here so nothing reads a deleted field). No `deformed: bool` is added beside a
  surviving `skinned: bool` — the one flag's bind meaning is generalized.
- **`submit_draw_list_skinned` is deleted and replaced by `submit_draw_list_deformed`** with the
  `morph_weights` parameter; the `renderer.rs` forwarder and the host/assets callers move to it. No
  skinned-only entry point survives.
- **The skin pass's `RgUsage` on the deformed buffer changes from write-only ordering to a declared
  `StorageReadCompute`** so the graph derives the morph→skin barrier; the stale "compute-write →
  vertex-input" framing in the skin-pass comment is rewritten to describe the compute→compute edge. No
  hand-written barrier is added.
- **`DrawItem` gains required fields with no `Default`**, so every literal (the three in
  `render_scene.rs`, the one in `instancing.rs`, and the test literal in `pipelines.rs`) is updated in the
  same change — the compile is the gate that no stale literal survives.

## Implementation-time direct reads (flag for the implementer)

- **`scene_pass.rs:bind_batch_vertices` match arms** — read the exact `match (batch.skinned, deformed)` block
  (`scene_pass.rs:bind_batch_vertices`) and all five call sites directly before editing; a morph-only batch
  that silently binds its static stream renders undeformed. Confirm the rename touches every arm.
- **`aa.rs:record_motion`** (`aa.rs:record_motion`, the `match (batch.skinned, deformed, prev_deformed)`) — the
  rename lands here in Phase 4; the prev-morph wiring is Phase 5. Read the match directly so a morph-only
  batch binds the deformed/prev-deformed buffers rather than the static stream.

## Test gate

- `cargo test -p saffron-rendering` `morph_kernel_deforms_to_golden_validation_clean` — a `cfg(test)` unit
  test mirroring `skinning.rs::tests::skin_kernel_deforms_to_golden_validation_clean`: upload a known base
  mesh + two sparse morph targets + a known weight vector, run the two-pass kernel (clear → scatter →
  resolve) on llvmpipe, and assert each deformed position equals `base + Σ wᵢ·δposᵢ` to a tight epsilon, the
  normal is renormalized, uv0 passes through, and `validation_issue_count()` is unchanged (the descriptor /
  pipeline / dispatch wiring is real-GPU-valid). Because the scatter uses integer atomics, the readback is
  bit-deterministic — assert against committed golden bytes, not just an epsilon ball.
- A `cfg(test)` const-assertion that `MorphFixedScale` keeps the golden test's max `Σ weight·delta` magnitude
  inside `i32` range and within the asserted round-trip error bound.
- `cargo test -p saffron-rendering` a `render_graph.rs` barrier-derivation unit test: declare
  `StorageWriteCompute` then `StorageReadCompute` on one imported buffer across two compute passes and assert
  the graph derives exactly one COMPUTE→COMPUTE WRITE→READ memory barrier (the morph→skin edge), per the
  `apply_access` hazard rule.
- A compaction unit test: a weight vector with sub-threshold and above-threshold entries produces the
  expected `ActiveTarget` list (below-threshold dropped, order stable), and an all-rest-pose weight vector
  compacts to empty (the morph dispatch is skipped).
- Milestone gate: `just engine` then `just prepare-for-commit` (format + `clippy -D warnings`), fixing every
  warning this change raises.
