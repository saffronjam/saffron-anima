# Phase 1 — AnimTrack/AnimClip generalization + .sanim v2 + sampler

**Status:** NOT STARTED

**Depends on:** Phase 0 (parenting foundation — the per-node entity forest). Nothing in this phase
spawns or binds nodes; it generalizes the *data model*, the *format*, and the *sampler* so the later
import (Phase 2), runtime (Phase 4), and storage (Phase 3) phases have a single track type and a single
evaluator to target.

## Goal

One generalized track model — `AnimTrack` carries `Target{Bone, Node}`, a `Path::Weights` channel, a
`u32 morphCount`, and `jointName` renamed to `targetName` — so a bone rotation, a node translation, and
an N-wide morph-weights stream are all the *same* struct. The `.sanim` format is **replaced** at v2 to
carry it (v1 is rejected with `Err`, no dual-version branch). One mode-keyed sampler gains a `Weights`
branch that reuses the existing STEP/LINEAR/CUBICSPLINE core per morph target, with the exact 3·N·M
CUBICSPLINE memory layout. There is no second evaluator and no second format.

This is the load-bearing seam for everything downstream: Phase 2 decodes glTF weights/non-joint/sparse
channels into this exact struct, Phase 4 routes the sampled value to bone/node `PoseOverride` or mesh
`MorphWeightOverride`, and Phase 3 stores the deltas the weights drive. Get the layout and the rename
right here and the rest is wiring.

## NO-LEGACY framing

- The rename `jointName → targetName` is **atomic across every reader in this single change**. A
  half-renamed tree does not compile (the field is referenced by value, not by string). The exact
  callsite set is enumerated in step 3 — there are only three real ones plus the two serde sites. The
  control DTO does **not** reference the field today (`AnimationClipDto` carries only a track *count*),
  so this rename does **not** ripple into the wire this phase.
- The `.sanim` version bump **replaces** the reader. `loadAnimationFromBytes` reads **v2 only**; a v1
  buffer returns `Err`. No `if (version == 1) … else if (version == 2)` branch. Migrating on-disk v1
  clips is out of scope (reimport regenerates them).
- The CUBICSPLINE-on-weights layout is **the** layout (the glTF `3·N` `[in, value, out]` stride), not an
  engine-private packing — so Phase 2's `cgltf_accessor_unpack_floats` output drops straight in and the
  byte-exact self-test pins it.

## Grounding (verified line numbers)

- `AnimTrack` / `AnimClip`: `geometry.cppm:76-110` — `i32 joint`, `std::string jointName`, `Path ∈
  {Translation, Rotation, Scale}`, `Interp ∈ {Step, Linear, CubicSpline}`, flat `times`/`values`.
- `toTrackPath`: `geometry.cppm:514-526`. `cgltf_animation_path_type_weights` exists at
  `third_party/cgltf/cgltf.h:229`.
- `.sanim`: `SANimHeader` (32 B, `:406-415`), `SANimTrackRecord` (20 B with a `u16 pad`, `:418-428`),
  `AnimFormatVersion = 1` (`:430`), `saveAnimationToBuffer` (`:1619-1650`), `loadAnimationFromBytes`
  (bounds-checked `take` cursor, `:1657-1731`; the version guard is at `:1669`, the record memcpy at
  `:1699-1705`).
- Sampler: `sampleTrack` returns `glm::vec4` (`animation.cpp:352-451`); `sampleClip`
  (`animation.cpp:453-480`); `sampleClipResolved` (jointName fallback, `:308-349`).
- Self-tests: `runAnimationSelfTest` (`animation.cpp:766+`), with the `.sanim` round-trip block at
  `:894-940` and the vec4 cubic case at `:812-827`.
- The only real `AnimTrack.jointName` callsites: `geometry.cppm` (struct + decode `:1076` + serde
  `:1641,1645,1712`), `animation.cpp` (`sampleClipResolved` `:316,320`; round-trip self-test
  `:899-937`), `assets.cppm` bake self-test `:4571`. The `jointName` lambdas at `gen.ts:3087` and
  `scene_component_serde.generated.cpp:501` are unrelated `BonePhysics::Joint` helpers — do not touch.

## Ordered steps

All file paths are absolute.

### 1. Generalize `AnimTrack` (the data model)

File: `/var/home/saffronjam/repos/SaffronEngine/engine/source/saffron/geometry/geometry.cppm`,
`struct AnimTrack` at lines 76–101.

Add the target kind enum, the `Weights` path, rename the binding field, and add the morph count. Keep
the existing `///` tone (say what it is now, no change-journey notes):

```cpp
/// One animated channel: a sampled curve targeting a bone or node's TRS, or a mesh's
/// morph-target weights. A lossless mirror of a glTF animation channel + sampler, bound
/// to its target by stable index plus the durable node name.
struct AnimTrack
{
    enum class Target : u8
    {
        Bone,  // joint indexes SkinnedMeshComponent.bones; written to a bone PoseOverride
        Node,  // a node-forest entity; written to that node's PoseOverride
    } target = Target::Bone;
    /// Stable index into SkinnedMeshComponent.bones for a Bone track (resolved at import
    /// by name); -1 for Node and Weights tracks (those bind by name only).
    i32 joint = -1;
    /// The glTF node name — the durable binding key (survives reorder/reimport).
    std::string targetName;
    enum class Path : u8
    {
        Translation,
        Rotation,
        Scale,
        Weights,  // N morph-target weights per key; N == morphCount
    } path = Path::Translation;
    enum class Interp : u8
    {
        Step,
        Linear,
        CubicSpline,
    } interp = Interp::Linear;
    /// Morph-target count N for a Weights track (the per-key channel width); 0 otherwise.
    u32 morphCount = 0;
    std::vector<f32> times;   // sampler.input — strictly increasing, seconds
    std::vector<f32> values;  // sampler.output — flat per key: vec3 (T/S), quat xyzw (R),
                              // or N floats (Weights). CubicSpline triples each block
                              // [in, value, out].
};
```

Notes: keep `joint` as `i32` (-1 for Node/Weights) — do **not** repurpose it. `target` defaults to
`Bone` so existing bone tracks read identically. `AnimClip` (`:103-110`) is unchanged structurally.

### 2. Map the glTF weights path

File: same module, `toTrackPath` at lines 514–526. Add the weights case:

```cpp
if (path == cgltf_animation_path_type_weights)
{
    return AnimTrack::Path::Weights;
}
```

Decoding the actual weights values from the accessor — and lifting the import gate that today skips
weights/non-joint channels (`geometry.cppm:1043`) — is **Phase 2**. This phase only makes the path
representable; no second mapping function.

### 3. Rename `jointName → targetName` atomically (every reader, one change)

1. `/var/home/saffronjam/repos/SaffronEngine/engine/source/saffron/geometry/geometry.cppm`
   - the struct field (step 1)
   - the import decode assignment at line 1076 (`track.jointName = …node…name;`)
   - the `.sanim` writer at lines 1641 and 1645
   - the `.sanim` reader at line 1712
2. `/var/home/saffronjam/repos/SaffronEngine/engine/source/saffron/animation/animation.cpp`
   - `sampleClipResolved` (`:316`, `:320`)
   - the `.sanim` round-trip self-test (`:899-937`): field inits and the `a.jointName == b.jointName`
     comparison
3. `/var/home/saffronjam/repos/SaffronEngine/engine/source/saffron/assets/assets.cppm`
   - the bake self-test at line 4571: `AnimTrack{ .joint = 1, .jointName = "joint" }` →
     `.targetName = "joint"`

After this, `grep -rn "jointName" engine/` returns **only** the two unrelated `BonePhysics` lambdas.
That grep being clean of any `AnimTrack` reference is part of acceptance. The per-channel
`targetName`/`kind` wire metadata is a **Phase 7** command (`list-clip-bindings` / channel makeup); no
frontend or DTO change here.

### 4. Generalize the sampler (`Weights` branch, one core)

File: `/var/home/saffronjam/repos/SaffronEngine/engine/source/saffron/animation/animation.cpp`,
`sampleTrack` at lines 352–451.

`sampleTrack` returns `glm::vec4` (3 floats for T/S, quat xyzw for R), which cannot carry an N-wide
weights block. Keep `sampleTrack(track, t) -> glm::vec4` for T/R/S **exactly as is** (its self-tests
still pin it) and add a weights sampler:

```cpp
/// Sample a Weights track at time t into `out` (resized to track.morphCount). STEP holds
/// the previous key, LINEAR lerps per target, CUBICSPLINE is the dt-scaled Hermite per
/// target; t is clamped to [first key, last key]. No normalize (weights are scalars, not
/// a quaternion). `out` is sized to N and zero-filled for a degenerate (non-Weights or
/// N == 0 or empty) track.
void sampleWeights(const AnimTrack& track, f32 t, std::vector<f32>& out);
```

Declare it in `animation.cppm` next to `sampleTrack` (`:76-85`) with the same `///` tone.

Implementation — reuse the same key search, `local`/`dt`, and Hermite basis as the vec4 path; the only
difference is the per-target index arithmetic and the channel width N:

- `N = track.morphCount`. If `track.path != Path::Weights || N == 0`, resize `out` to N and zero-fill,
  return. Else resize `out` to N.
- `n = times.size()`. Empty → zero-fill `out`, return.
- Clamp: `t <= times.front()` → key 0; `t >= times.back()` → key n-1; else `upper_bound`/`i0`/`i1`/
  `local`/`dt` exactly as the vec4 path (`:414-422`).
- **STEP/LINEAR layout** — per-key block is N contiguous floats, so target j at key k is
  `values[k*N + j]`:
  - STEP: `out[j] = values[i0*N + j]`.
  - LINEAR: `out[j] = mix(values[i0*N + j], values[i1*N + j], local)`.
- **CUBICSPLINE layout** — each key is a `3·N` block laid `[in[N], value[N], out[N]]`, so a target's
  triplet is **not** contiguous; for target j at key k:
  - in-tangent  `values[k*3N + j]`
  - value       `values[k*3N + N + j]`
  - out-tangent `values[k*3N + 2N + j]`

  Per target j, with `h00,h10,h01,h11` from `local` (`:442-445`) and dt-scaled tangents (matching the
  vec4 path `:448-449`):

  ```
  p0 = values[i0*3N + N + j]
  p1 = values[i1*3N + N + j]
  m0 = values[i0*3N + 2N + j] * dt   // out-tangent of the start key
  m1 = values[i1*3N + j]       * dt   // in-tangent  of the end key
  out[j] = h00*p0 + h10*m0 + h01*p1 + h11*m1
  ```

  No quaternion normalize, no slerp — weights are independent scalars.

  Factor the shared `local`/`dt`/Hermite-coefficient math so the vec4 path and the weights path read the
  **same** formula (two divergent Hermite implementations would be a second evaluator). A small `static`
  helper in the anonymous namespace returning `{i0, i1, local, dt}` from `(times, t)` is the cleanest
  shared seam; both samplers call it.

`sampleClip`/`sampleClipResolved` route a `Node` T/R/S track exactly like a bone track (same
`sampleTrack`). Where a `Weights` track's value is *written* (mesh `MorphWeightOverride`) is **Phase 4**;
this phase only makes the value samplable. Do not add a weights destination to `PoseBuffer` until Phase 4
needs it — the canonical design writes morph weights to a per-mesh `MorphWeightOverrideComponent`, not
into the joint-sized `PoseBuffer`, so a `PoseBuffer.weights*` field would be dead this phase. (If a later
phase finds a `PoseBuffer` weights layer is genuinely needed, add it then, sized by its caller.)

### 5. Replace the `.sanim` format at v2

File: `/var/home/saffronjam/repos/SaffronEngine/engine/source/saffron/geometry/geometry.cppm`.

1. **Version bump** (line 430): `inline constexpr u32 AnimFormatVersion = 2;` (replace; no second
   constant).

2. **`SANimTrackRecord`** (lines 417–428). Repurpose the `u16 pad` into `{u8 target; u8 reserved}` and
   add `u32 morphCount`. The record grows 20 → 24 bytes:

   ```cpp
   // 24-byte per-track record; the target name, times, then values follow it. `reserved`
   // is zero padding so the record stays 4-byte aligned for the bounds cursor.
   struct SANimTrackRecord
   {
       i32 joint;
       u8 path;
       u8 interp;
       u8 target;    // AnimTrack::Target
       u8 reserved;  // 0
       u32 nameLen;
       u32 timeCount;
       u32 valueCount;
       u32 morphCount;
   };
   static_assert(sizeof(SANimTrackRecord) == 24, "SANimTrackRecord must be exactly 24 bytes");
   static_assert(alignof(SANimTrackRecord) == 4, "SANimTrackRecord must be 4-byte aligned");
   ```

   Re-assert exact size **and** alignment — the cursor at line 1699 (`take(sizeof(SANimTrackRecord))`)
   memcpys exactly `sizeof` bytes; a wrong size reads past the record. The field order keeps every member
   naturally aligned with no tail padding (i32 + 4×u8 + 4×u32, all 4-aligned), so `sizeof == 24` holds
   without `#pragma pack`; if a compiler inserts padding, reorder rather than pack. `SANimHeader`
   (`:406-415`) is **unchanged** (32 bytes; three reserved u32 still spare).

3. **Writer** `saveAnimationToBuffer` (`:1619-1650`): set `record.target = static_cast<u8>(
   track.target)`, `record.reserved = 0`, `record.morphCount = track.morphCount`, and write
   `track.targetName` where it wrote `track.jointName`. Times/values appended after the name are
   identical.

4. **Reader** `loadAnimationFromBytes` (`:1657-1731`): the version guard at line 1669 already rejects
   any `header.version != AnimFormatVersion`, so bumping the constant to 2 **automatically** rejects v1
   with the existing `Err(std::format("unsupported animation version {}", header.version))` — the
   NO-LEGACY cutover, no new branch. In the per-track loop (after the memcpy at `:1705`):
   - `track.target = static_cast<AnimTrack::Target>(record.target);`
   - `track.morphCount = record.morphCount;`
   - assign `track.targetName` from the name bytes (`:1712`, renamed).

   The `take`-based cursor handles the 24-byte record transparently.

5. **`.smodel` is unchanged.** It embeds the `.sanim` bytes verbatim as a chunk payload by fourcc TOC
   (`writeContainer`/`SModelHeader` at `:1756+`), opaque to the container, so `ContainerFormatVersion`
   does **not** bump. Confirm no container-level assertion mentions `.sanim` length.

### 6. Extend the self-tests (byte-exact)

File: `/var/home/saffronjam/repos/SaffronEngine/engine/source/saffron/animation/animation.cpp`,
`runAnimationSelfTest` (`:766+`). Reuse the in-scope `expect`/`eps` harness.

1. **STEP weights (`k*N+j` slicing), N > 4.** A 5-wide track, 2 keys, STEP. Just before key 1's time the
   result equals key 0's N values; at key 1's time it equals key 1's N values.

   ```cpp
   AnimTrack w;
   w.path = AnimTrack::Path::Weights;
   w.interp = AnimTrack::Interp::Step;
   w.morphCount = 5;
   w.times = { 0.0f, 1.0f };
   w.values = { 0,0,0,0,0,   1,2,3,4,5 };  // key0: zeros, key1: 1..5
   std::vector<f32> out;
   sampleWeights(w, 0.9f, out);  // expect {0,0,0,0,0}
   sampleWeights(w, 1.0f, out);  // expect {1,2,3,4,5}
   ```

2. **LINEAR weights (`k*N+j` slicing), N > 4.** Same shape, LINEAR; at `t = 0.5` expect
   `{0.5, 1.0, 1.5, 2.0, 2.5}`. Also assert clamp below/above the ends.

3. **CUBICSPLINE on weights, byte-exact (load-bearing).** N = 5, 2 keys, the `[in[N], value[N], out[N]]`
   per-key block. Replicate the vec4 cubic curve (`:812-827`, which reaches 0.75 at the midpoint for
   value0=0, value1=1, out-tangent0=2, in-tangent1=0 over dt=1) on **target 0** inside the N-wide block,
   leaving the others neutral, so the layout indexing is what's under test:

   ```cpp
   AnimTrack w;
   w.path = AnimTrack::Path::Weights;
   w.interp = AnimTrack::Interp::CubicSpline;
   w.morphCount = 5;          // N = 5
   w.times = { 0.0f, 1.0f };
   w.values = {               // per key: in[5], value[5], out[5]
       0,0,0,0,0,  0,0,0,0,0,  2,0,0,0,0,   // key0: in, value, out(target0 out-tan = 2)
       0,0,0,0,0,  1,0,0,0,0,  0,0,0,0,0,   // key1: in, value(target0 = 1), out
   };
   std::vector<f32> out;
   sampleWeights(w, 0.5f, out);
   // expect out[0] == 0.75, out[1..4] == 0
   ```

   Hand-computed: dt=1, `local=0.5` → `h00=0.5, h10=0.125, h01=0.5, h11=-0.125`; `p0=0, p1=1, m0=2*1=2,
   m1=0*1=0` → `0.125*2 + 0.5*1 = 0.25 + 0.5 = 0.75`. Assert `glm::abs(out[0]-0.75f) < eps` and the
   other four `< eps`.

4. **`.sanim` v2 round-trip with a Node track and a Weights track (N > 4).** Extend the existing block
   (`:894-940`). Add a `Target::Node` track (a translation track with `target = Node`, `joint = -1`,
   non-empty `targetName`) and a `Path::Weights` track (`morphCount = 6`, ≥2 keys). After save→load,
   compare `a.target == b.target`, `a.morphCount == b.morphCount`, `a.targetName == b.targetName`, plus
   the existing `path/interp/times/values` equality.

5. **v1 rejection.** Build a minimal valid-magic buffer (`SANimHeader` with `magic="SANM"`, `version=1`,
   `trackCount=0`) and assert `loadAnimationFromBytes(buf)` returns `Err`. Pins that v1 is rejected, not
   silently parsed.

`runAnimationSelfTest` is already wired into the headless self-test gate; no new call site. The existing
T/R/S `sampleTrack` self-tests (`:784-892`) must stay green unchanged — proof the shared-Hermite refactor
did not regress the vec4 path.

## Frontend (Timeline / Clips / Inspector)

**None this phase.** No wire field changes, no new command. The `targetName` rename and the per-channel
`kind`/`target` metadata surface to the editor only when Phase 7 adds the channel-makeup command and
regenerates `@saffron/protocol`. Do not pre-emptively touch `editor/`.

## Performance

- `sampleWeights` is O(active-keyframes × N) per Weights track per clip per frame on the CPU — same order
  as TRS sampling. A 100-shape rig with one active key pair costs ~100 mul-adds/frame: negligible. No GPU
  work; no allocation in the hot path if the caller reuses the `out` vector (Phase 4 owns it).
- The `.sanim` record grows 20 → 24 bytes per track. Trivial on-disk delta; no runtime cost.

## Control commands

**None this phase.** Morph/binding/channel commands are Phase 7. The drivable state they expose
(`MorphComponent`, node players) lands in Phase 3/4; this phase is pure data model + format + sampler.

## Docs to update (same change)

1. `/var/home/saffronjam/repos/SaffronEngine/docs/content/explanations/animation/animation-data-model.md`
   — document the `AnimTrack` `Target{Bone, Node}` kind, the `Path::Weights` channel, the
   `jointName → targetName` rename, `morphCount`, and the weights sampler semantics (per-target STEP/
   LINEAR/CUBICSPLINE, the `k*N+j` contiguous layout vs the `3·N` `[in, value, out]` CUBICSPLINE layout,
   no normalize). Keep the slim `What | File | Symbols` table on `AnimTrack`/`sampleTrack`/`sampleWeights`
   (symbols, not line numbers).
2. `/var/home/saffronjam/repos/SaffronEngine/docs/content/explanations/geometry-and-assets/sanim-format.md`
   — bump the documented format to v2: the new 24-byte `SANimTrackRecord` layout
   (`i32 joint; u8 path; u8 interp; u8 target; u8 reserved; u32 nameLen; u32 timeCount; u32 valueCount;
   u32 morphCount`), the `targetName` rename, that v1 files are rejected with `Err` (no migration), and
   that `.smodel`'s container version is unchanged because it embeds `.sanim` verbatim.

Both are edits (no new page). Touch the animation hub `_index.md` row only if a sentence there describes
the track model.

## Tests

In `runAnimationSelfTest` (the headless gate), per step 6:
- STEP weights `k*N+j` slicing (N=5).
- LINEAR weights `k*N+j` slicing + clamp (N=5).
- **Byte-exact** CUBICSPLINE-on-weights (hand-computed 0.75 Hermite, N=5, the `3·N` `[in, value, out]`
  indexing).
- `.sanim` v2 round-trip including a `Target::Node` track and a `Path::Weights` track (N=6).
- v1 `.sanim` buffer → `Err`.

The existing vec4 T/R/S self-tests stay green unchanged.

**tests/e2e:** none this phase — there is no control/wire surface to drive yet. The e2e fixtures for
morph/node animation land in Phase 9, once the commands (Phase 7) and frontend (Phase 8) exist.

## Acceptance criteria

- `AnimTrack` carries `Target{Bone, Node}`, `Path::Weights`, `targetName`, and `u32 morphCount`; the
  default `Target::Bone` keeps existing bone tracks reading identically.
- Every reader of the old `jointName` is updated in this one change: `grep -rn "jointName" engine/`
  returns only the two unrelated `BonePhysics::Joint` lambdas — no `AnimTrack` reference remains.
- `toTrackPath` maps `cgltf_animation_path_type_weights → Path::Weights`.
- The byte-exact CUBICSPLINE-on-weights self-test passes (out[0] == 0.75 ± eps); the STEP and LINEAR
  weights `k*N+j` slicing self-tests pass; the vec4 T/R/S self-tests still pass.
- `.sanim` writes/reads v2 with Node and Weights tracks (round-trip self-test passes); a v1 buffer is
  rejected with `Err`.
- `SANimTrackRecord` re-asserts its exact size (24) **and** 4-byte alignment via `static_assert`.
- `.smodel` `ContainerFormatVersion` is unchanged (it embeds `.sanim` verbatim).
- Docs `animation-data-model.md` and `sanim-format.md` reflect the Target kind, Weights path,
  `targetName` rename, `morphCount`, and the v2 record layout.
- `make engine` then `make prepare-for-commit` (format + lint) are clean for this change.
