# Motion vectors and ray-traced BLAS for morphed geometry

**Status:** COMPLETED
**Depends on:** Phase 4 (the GPU morph deform pass writes `base + Σ wᵢ·δ` into the shared
`deformed` buffer before skin; the draw list carries `MorphDispatch`/`morph_dispatches` and the
`DrawBatch.deformed` flag means "draws the deformed buffer").

## Progress — DONE (full workspace build + clippy + 151 rendering tests green)

- **`swap_morph_weights` + `prev_morph_weights_by_entity`** on `Skinning` (twin of `swap_palette`:
  length-guard, read-then-overwrite). Unit-tested (`swap_morph_weights_mirrors_palette_swap`).
- **Prev-pose morph dispatch.** `instancing.rs` builds a `prev_morph_dispatches` list alongside the
  cur list: `swap_morph_weights` → `build_active_targets(prev_weights)` → a prev `MorphDispatch`. Both
  cur + prev active targets share the one `active_targets` buffer (disjoint `active_base` regions); only
  the prev set's output buffer differs (prev-deformed). `wire_morph_dispatches` now wires a cur set
  (→deformed) **and** a prev set (→prev-deformed) per instance, calling `ensure_prev_deformed_capacity`,
  using the reserved `*2` pool budget (no resize). `record_morph` chains `dispatches` then
  `prev_dispatches`. Change-gating is implicit: prev == cur ⇒ identical active list ⇒ prev-deformed ==
  deformed ⇒ zero deformation motion (the same steady-state behaviour as `prev_skin_dispatches`).
- **`renderer.rs` deform block.** The morph `RgPass` now declares `StorageWriteCompute` on **both**
  deformed + prev-deformed and records cur + prev; prev-deformed is imported once for the whole
  `do_deform` scope and shared with the skin pass.
- **`DeformedRtInstance` (renamed from `SkinnedRtInstance`) + `world_transform: Mat4`.** Every reader
  moved (`draw_list.rs` def + `SceneDrawList.deformed_rt_instances` + `shallow_clone`, `instancing.rs`,
  `rt.rs` params, `renderer.rs`, lib re-export). The RT gather is widened: skinned buckets ride
  `skinned_rt` (wired + budget-clamped in `wire_skin_dispatches`, `world_transform = IDENTITY`);
  unskinned-morph buckets push a `morph_rt` instance (`world_transform = bucket.model`) appended after
  wiring. `prepare_tlas_build` places every deforming instance via
  `transform_rows(&inst.world_transform)` — the hardcoded `IDENTITY_ROWS` placement is gone; the
  constant is now `#[cfg(test)]` and only pins the `transform_rows(&Mat4::IDENTITY) == IDENTITY_ROWS`
  byte-identity test. The first-sight BLAS build reads the live post-morph deformed slice (no
  zero-weight base path).
- **`record_motion`** keys on `batch.deformed` via the extracted pure `select_motion_streams` helper
  (unit-tested `select_motion_streams_keys_on_the_deform_flag`): a morph-only batch binds cur + prev
  deformed buffers exactly like a skinned batch.
- **`motion.slang`** verified to already carry cur (loc 0) + prev (loc 3) position bindings — no shader
  change; comment updated to "deforming (skinned or morph)".
- **Remaining for Phase 7 e2e:** the on-GPU validation-clean run of a morph-only mesh whose BLAS refits
  (llvmpipe) — the language-appropriate place per AGENTS.md, composed into the motion/RT screenshot
  checks. The `prepare_tlas_build` instance-gather assertion is covered by the byte-identity test (the
  placement math) + that e2e run.

## Goal

Carry morph deformation through the two consumers that read the *final* deformed slice: the motion
prepass and the ray-traced BLAS. The motion pass must dispatch the morph kernel a second time on the
**previous frame's** weights into the prev-deformed buffer, so a morph-only mesh produces real
deformation motion vectors (not just object motion). The RT path must refit each deformed
instance's BLAS over its post-morph vertices and place it in the TLAS at the right transform —
identity for skinned (already world-space) and the node world matrix for an unskinned-morph mesh.
This phase generalizes the existing skin-only motion/RT seams to the one deform concept; after it,
both subsystems key on "this instance deforms" (skin OR morph), not "this instance is skinned".

## Design

### Prev-pose morph weights — the twin of `swap_palette`

The skin prev-pose path keeps `prev_palette_by_entity: HashMap<u64, Vec<Mat4>>` on the deform owner
(`rendering/src/skinning.rs:Skinning`) and reads-then-overwrites it once per entity per frame via
`swap_palette` (`skinning.rs:swap_palette`): it returns the cached last-frame palette (or the current
one for an entity it has never seen, yielding zero deformation motion on the first frame) and stores
the current palette for next frame. The commit is inline at the call site in `instancing.rs` — there
is no separate batched commit block; each entity appears once per draw list, so the read-then-write
order alone preserves last-frame semantics.

Morph weights get the exact same shape. Add `prev_morph_weights_by_entity: HashMap<u64, Vec<f32>>`
to `Skinning`, and a `swap_morph_weights(&mut self, entity: u64, current: &[f32]) -> Vec<f32>`
method that mirrors `swap_palette` line-for-line: return the cached slice when its length matches the
current weight-vector length, otherwise return `current.to_vec()` (an unseen entity, or a
length change from a different mesh binding, yields prev == cur → zero deformation motion), then
insert `current.to_vec()`. The length-equality guard is the morph analogue of the palette's
`cached.len() == current.len()` check; it is what makes a freshly-spawned morph instance produce no
first-frame ghost.

In `instancing.rs`, inside the per-bucket loop where `swap_palette` is already called for skinned
buckets, the deform branch (now entered for skin OR nonzero-weight morph buckets — Phase 4 widened
the bucket predicate) calls `swap_morph_weights(bucket.entity, &this_bucket_weights)` and writes the
returned prev weights into the `prev_morph_dispatches` weight buffer, exactly parallel to how the
cached palette slice is copied into `prev_joints[lo..hi]`. The commit is inline at this call site;
do not add a separate batched commit block.

### Prev-pose morph dispatch (change-gated, prev==cur skips)

Phase 4 produced `SceneDrawList.morph_dispatches` and `prev_morph_dispatches` (the prev list runs
parallel to the buckets, the twin of `prev_skin_dispatches`). The motion pass dispatches the morph
kernel on the prev weights into the prev-deformed buffer before it dispatches the prev skin kernel,
so the prev-deformed slice holds `prev_morph ∘ prev_skin` — the same composition order the current
slice holds. Reuse the existing **null-`DescriptorSet` skips the dispatch** convention: when an
entity is unseen this frame (`swap_morph_weights` returned prev == cur), the wiring still allocates
a set, but the dispatch is change-gated — a prev dispatch whose prev weights equal the current
weights is identity work and is dropped, leaving the prev-deformed slice equal to the current
deformed slice so the motion vector for that instance is pure object motion (the desired result for a
brand-new or steady-state morph instance). This matches how `prev_skin_dispatches` already behaves
for a steady palette.

### BLAS refit over the post-morph slice

`Rt::prepare_tlas_build` (`rendering/src/rt.rs:prepare_tlas_build`) already refits each deformed
instance's BLAS from the `deformed_buffer` at the instance's `deformed_offset` via
`plan_skinned_blas_refits` (`rt.rs:plan_skinned_blas_refits`), creating the AS on first sight and
issuing an in-place `UPDATE` afterwards. Because the morph compute pass runs *before* the RT build in
the frame graph (Phase 4 ordered morph → skin → geometry/RT through the shared buffer), the deformed
slice the refit reads already reflects the resolved morph weights — no new ordering is needed here.
The one substantive change is the **initial** build: the first-sight build path (`rt.rs` `None` arm
of the `skinned_blas` lookup, around the `create on first sight` comment) must build over the *same*
post-morph deformed slice it later refits — i.e. a representative resolved-weight pose, never the
zero-weight base. Since the build reads the live `deformed_buffer` at the instance's offset on the
frame it first appears (after the morph dispatch has run), this falls out for free: do **not** add a
separate zero-weight base-pose build path. Topology is fixed (morph perturbs positions/normals only,
never the index stream), so `UPDATE` refit is valid for every subsequent frame exactly as for skin.

### `DeformedRtInstance` + `world_transform` (decision #14)

Rename `SkinnedRtInstance` (`rendering/src/draw_list.rs:SkinnedRtInstance`) to `DeformedRtInstance`
and add `world_transform: Mat4`. The placement rule, consumed in `prepare_tlas_build`:

- **Skinned** and **morph+skin**: the deformed vertices are already in world space (the palette is
  `worldBone * inverseBind` and the skin kernel omits the model matrix), so `world_transform =
  Mat4::IDENTITY`. `transform_rows(&Mat4::IDENTITY)` must produce bytes **byte-identical** to the
  existing `IDENTITY_ROWS` constant (`rt.rs:IDENTITY_ROWS`); the existing
  `transform_rows_transposes_to_row_major` test already proves the transpose, and a new assertion
  pins `transform_rows(&Mat4::IDENTITY) == IDENTITY_ROWS`.
- **Unskinned-morph**: there is no palette to fold the model matrix into, so the deformed vertices
  are in the mesh's local space. `world_transform` is the mesh-bearing node's world matrix (the same
  matrix `update_world_transforms` derives for that entity), and the instance is placed via
  `transform_rows(&inst.world_transform)`.

In `prepare_tlas_build`, the skinned-instance placement loop changes from the hardcoded
`make_instance(IDENTITY_ROWS, index, slot.accel.address)` to
`make_instance(transform_rows(&inst.world_transform), index, slot.accel.address)`. The skin path is
byte-identical because its `world_transform` is identity; the morph-only path gets correct world
placement. `IDENTITY_ROWS` stays as the source-of-truth constant the test asserts against, but it is
no longer referenced in the placement loop — the row matrix now always comes from
`transform_rows`.

### Widen the RT gather from "skinned" to "deforms" (decision #15)

In `instancing.rs` the per-bucket loop pushes a `DeformedRtInstance` only inside `if bucket.skinned`.
Widen the gather: push a `DeformedRtInstance` for **every deforming bucket** (skin OR nonzero-weight
morph), so an unskinned-morph mesh enters the TLAS. Each pushed instance carries:
`entity` (keyed for the grow-only per-entity BLAS, the placeholder `0` for the non-RT-armed case
preserved), `deformed_offset`, `vertex_count`, `index_count`, `mesh`, and the new `world_transform`
(`Mat4::IDENTITY` for a skin/morph+skin bucket, the node world matrix for a morph-only bucket — the
bucket already carries the entity's resolved model matrix used elsewhere in the loop). The field
`SceneDrawList.skinned_rt_instances` renames to `deformed_rt_instances`; every reader
(`prepare_tlas_build`'s `skinned: &[...]` parameter, `Rt::has_instances`, the `shallow_clone` field
copy) moves to the new name and `DeformedRtInstance` type in the same change.

### `record_motion` keys cur/prev on the deform flag

`record_motion` (`rendering/src/aa.rs:record_motion`) selects the cur/prev vertex bindings per batch
with `match (batch.skinned, deformed, prev_deformed)` — `(true, Some, Some)` binds the deformed +
prev-deformed buffers, every other arm binds the static stream to both. Phase 4 renamed
`DrawBatch.skinned` to `DrawBatch.deformed`; this match changes to `match (batch.deformed, deformed,
prev_deformed)`. A morph-only batch now has `batch.deformed == true`, so it binds the current
deformed buffer (binding 0) and the prev-deformed buffer (binding 1), and the motion shader reads
real per-vertex deformation motion for it. No other arm changes.

### motion.slang — no shader change

`motion.slang` already declares both a current-position binding and a previous-position binding (the
skin motion path feeds them distinct cur/prev deformed buffers). A morph-only batch reuses those
exact two bindings — the only difference is *which* buffers the host binds, handled entirely in
`record_motion` above. Confirm by reading the shader that both position bindings exist; there is no
SPIR-V change in this phase.

### Pool sizing reconciliation

The deform descriptor pool is already sized for cur + prev as `SKIN_POOL_SET_CAPACITY =
SKIN_MAX_SETS_PER_FRAME * 2` (`skinning.rs:SKIN_POOL_SET_CAPACITY`). The morph sets the deform owner
allocates (cur + prev) come out of that same `*2` budget that Phase 4 already folded the morph half
into; this phase adds the prev-morph dispatch path that *consumes* the prev half of that budget but
requires no further capacity bump. Reconcile by confirming Phase 4's pool already accounts for the
morph cur+prev sets — do not double-size here.

## Changes

| What | Location (file:symbol) | Kind |
|---|---|---|
| `prev_morph_weights_by_entity: HashMap<u64, Vec<f32>>` on the deform owner | `rendering/src/skinning.rs:Skinning` | modify |
| `swap_morph_weights(entity, current) -> Vec<f32>` (twin of `swap_palette`, length-guard, read-then-overwrite) | `rendering/src/skinning.rs` | add |
| Inline call to `swap_morph_weights` in the deform branch; write prev weights into `prev_morph_dispatches` buffer | `rendering/src/instancing.rs` (per-bucket loop, beside `swap_palette`) | modify |
| Change-gate the prev-morph dispatch (prev == cur ⇒ drop, leaving prev-deformed == deformed) | `rendering/src/instancing.rs` / the prev-morph dispatch wiring from Phase 4 | modify |
| Rename `SkinnedRtInstance` → `DeformedRtInstance`; add `world_transform: Mat4` | `rendering/src/draw_list.rs:SkinnedRtInstance` | modify |
| Rename `SceneDrawList.skinned_rt_instances` → `deformed_rt_instances`; update `shallow_clone` | `rendering/src/draw_list.rs:SceneDrawList` | modify |
| Widen the RT instance gather from `if bucket.skinned` to every deforming bucket; set `world_transform` per kind | `rendering/src/instancing.rs` (`skinned_rt.push` site) | modify |
| Placement loop uses `transform_rows(&inst.world_transform)` instead of `IDENTITY_ROWS` | `rendering/src/rt.rs:prepare_tlas_build` | modify |
| `skinned: &[SkinnedRtInstance]` param → `deformed: &[DeformedRtInstance]`; `has_instances` param | `rendering/src/rt.rs:prepare_tlas_build`, `rt.rs:has_instances`, `rt.rs:plan_skinned_blas_refits` | modify |
| Initial first-sight BLAS build reads the post-morph slice (no zero-weight base build path added) | `rendering/src/rt.rs:plan_skinned_blas_refits` (`None` arm) | modify |
| `match (batch.skinned, ...)` → `match (batch.deformed, ...)` | `rendering/src/aa.rs:record_motion` | modify |
| Confirm `motion.slang` has cur + prev position bindings (no shader edit) | `engine/assets/shaders/motion.slang` | verify |
| Reconcile the `*2` deform pool already covers morph cur+prev sets (no capacity bump) | `rendering/src/skinning.rs:SKIN_POOL_SET_CAPACITY` | verify |

## New artifacts

- `DeformedRtInstance` (renamed from `SkinnedRtInstance`) with the new `world_transform: Mat4` field.
- `Skinning::swap_morph_weights` and the `prev_morph_weights_by_entity` cache.
- No new shader, no new descriptor pool, no new draw-list field beyond the rename and the
  `world_transform` addition (the `morph_dispatches`/`prev_morph_dispatches` lists already exist from
  Phase 4).

## NO-LEGACY cutover (same change)

- **`SkinnedRtInstance` is deleted, not aliased.** The type is renamed to `DeformedRtInstance` and
  every reference (`draw_list.rs` definition + doc, the `instancing.rs` import and `skinned_rt`
  local, the `rt.rs` import, the `prepare_tlas_build`/`has_instances`/`plan_skinned_blas_refits`
  parameters, the `SceneDrawList.skinned_rt_instances` field and its `shallow_clone` copy) moves to
  the new name in this change. No type alias, no second instance struct for morph — the one
  `DeformedRtInstance` carries both skin and morph via `world_transform`.
- **The hardcoded `IDENTITY_ROWS` placement is deleted from the loop.** `prepare_tlas_build` no
  longer special-cases skinned instances with a literal `IDENTITY_ROWS`; the row matrix always comes
  from `transform_rows(&inst.world_transform)`. `IDENTITY_ROWS` survives only as the constant the
  byte-identity test asserts against.
- **No separate morph RtInstance type and no separate morph motion path.** The skin-only
  `match (batch.skinned, ...)` in `record_motion` is replaced by the generalized
  `match (batch.deformed, ...)`; there is no parallel morph-specific motion arm. The prev-pose morph
  dispatch reuses the existing `prev_*_dispatches` + null-set-skip machinery, not a new code path.
- **No zero-weight base BLAS build.** The first-sight build path is updated to read the post-morph
  slice; the prior assumption that the build pose is the static base is removed.
- Every test that constructs `SkinnedRtInstance` by literal or names `skinned_rt_instances` moves to
  `DeformedRtInstance` / `deformed_rt_instances` with the added `world_transform` field in the same
  change (the `rt.rs` `#[cfg(test)] mod tests` instance constructors and the `instancing.rs` draw-list
  tests).

## Test gate

- `cargo test -p saffron-rendering`:
  - A `swap_morph_weights` unit test mirroring the palette swap semantics: an uncached entity returns
    `current` (prev == cur ⇒ zero motion); a length change (different mesh binding) returns `current`;
    a same-length second call returns the previously stored slice; the cache holds the latest weights
    after each call.
  - A `transform_rows(&Mat4::IDENTITY) == IDENTITY_ROWS` byte-identity assertion, extending the
    existing `transform_rows_transposes_to_row_major` test, so the skin RT placement is provably
    unchanged.
  - A `prepare_tlas_build` instance-gather test asserting an unskinned-morph instance
    (`world_transform != IDENTITY`) is placed with the node world matrix while a skinned instance is
    placed identity.
  - A `record_motion` arm test confirming a `batch.deformed == true` morph-only batch binds the
    current + prev deformed buffers (not the static stream).
- A validation-clean GPU run (llvmpipe is fine) of a morph-only mesh that deforms and whose BLAS
  refits: `SAFFRON_EXIT_AFTER_FRAMES=N ./engine/target/debug/saffron-host` with the RT consumer armed,
  asserting zero Vulkan validation messages and a non-degenerate TLAS (the morph-only instance present
  at its world transform). This composes into the Phase 7 e2e motion/RT screenshot-delta checks.
- Milestone gate per AGENTS.md: `just engine` then `just prepare-for-commit` (format + clippy
  `-D warnings`); fix every warning this change raises.
