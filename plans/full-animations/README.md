# Full animation — morph targets + node-TRS animation — design

**Status:** NOT STARTED

The decision-locked design for two animation capabilities, built on Saffron's existing seams with the
smallest correct cutover (conventions-purist, minimal-footprint). It reconciles the prior on-disk plan
in place and incorporates the two correctness fixes a review surfaced — the cgltf sparse-decode
primitive and the N-wide CUBICSPLINE weights layout — plus the strongest ideas from the
performance/AAA-parity and editor-UX angles.

1. **Morph targets (blend shapes)** — full-fidelity POSITION+NORMAL sparse deltas (tangent re-derived
   at deform time, the UE/glTF approach), sparse-accessor decode, all three interpolation modes (incl.
   the N-wide CUBICSPLINE weights stream), arbitrary N targets, a GPU morph compute stage composed
   *before* the existing compute-skinning prepass, motion vectors + RT BLAS carried through, a
   `MorphComponent` + a runtime weight override, control commands, and the editor Timeline/Clips/
   Inspector drill-down.
2. **Node-TRS animation (non-skeletal)** — animate plain entity `TransformComponent`s from glTF
   translation/rotation/scale channels bound to nodes. The skin-only import gate
   (`geometry.cppm:944`) is **lifted** into one decode path; node tracks bind by durable name to a
   cached stable `Uuid`; and node-TRS rides the existing `PoseOverrideComponent` +
   `updateWorldTransforms` hierarchy with **zero new compose code**.

Both are **generalized, not cloned**, onto one `AnimTrack`/`AnimClip`/`sampleClip`/`tickAnimation`,
ride a Phase-0 parenting foundation (delete the multi-node flatten, instantiate a per-node entity
forest), and surface through the **existing** editor Timeline + Clips + Inspector (one clip = one bar,
channels nested) — no parallel UI.

This is **a plan only.** No engine/editor source is touched here. Each `phase-N-*.md` carries its own
`**Status:**` line and the concrete edits; this README is the locked design they implement.

---

## 1. What's missing today vs what this adds

Today the animation runtime is **skeletal only**. `AnimTrack` carries a `jointName` and a bone index;
`AnimClip` holds only bone tracks; `sampleClip`/`tickAnimation` write `PoseOverrideComponent`s onto
skinned-mesh bones. The glTF importer's animation + node-forest path is **gated behind a skin check**
(`geometry.cppm:944` — `data->skins_count > 0 && sawSkinnedPrimitive && !sawUnskinnedPrimitive`), and
multi-node unskinned models are **flattened**: `cgltf_node_transform_world` is baked into vertices
(`geometry.cppm:891`, `applyNodeTransform=true`), leaving no live local transforms to drive. The
importer also explicitly **skips** the `weights` channel, non-joint targets, and sparse accessors. So:

- a node-TRS glTF (e.g. `BoxAnimated`) imports as static baked geometry with its animation discarded;
- a morph glTF (e.g. `AnimatedMorphCube`, `MorphStressTest`) imports with its targets dropped — there
  is no morph storage, no morph deform stage, and no morph component or weight channel anywhere.

This plan adds, on the existing seams and with one code path per concern:

- **Node-TRS playback** — non-skeletal entities animate their local transform through the existing
  hierarchy; world falls out of the parent chain.
- **Morph targets** — sparse per-vertex deltas, a GPU morph deform stage composed before skin, durable
  + runtime-override weights, motion vectors, and RT BLAS support.
- a **parenting foundation** so an unskinned multi-node glTF spawns a live per-node entity forest;
- the **control commands** and **editor UI** that make both drivable and inspectable.

---

## 2. Architecture summary

Two animation capabilities land as **one coherent extension** of Saffron's existing seams, with
exactly one code path per concern (NO LEGACY).

**Morph targets.** `glTF mesh.primitives[].targets` decode into a **sparse per-vertex delta bank** —
`struct MorphDelta { u32 vertexIndex; vec3 dPosition; vec3 dNormal; }` (28 B, NORMAL-only; no
`dTangent`, because the engine `Vertex` (`geometry.cppm:36`) carries no tangent stream — the tangent is
re-derived by Gram-Schmidt against the morphed normal at deform time, the UE approach). The bank is
stored as a section in a flags-collapsed `.smesh` and uploaded once as a shared read-only SSBO. A new
`morph.slang` compute pass runs **immediately upstream** of the existing skin prepass — the
morph-before-skin canonical order is enforced **structurally**: the morph stage writes a `morphedBase`
buffer that **IS** the skin pass's input binding, so skin-first is physically impossible. The skin pass
adds an explicit `StorageReadCompute` access on `morphedBase` (its input is now GPU-written, unlike the
host-uploaded static stream that needs no barrier — the exact seam where graph-derived barriers would
otherwise silently fail). For an unskinned morph mesh the morph stage writes the deformed buffer
directly and the mesh draws from it as a static stream (one deformed-buffer contract). Per-entity
weights live in a durable `MorphComponent` (seeded `node.weights ?? mesh.weights ?? zeros`) with a
runtime-only `MorphWeightOverrideComponent` the evaluator writes each frame (mirrors
`PoseOverrideComponent`, removed on stop → non-destructive Edit preview by construction). Motion
vectors carry morph through a `prevMorphWeightsByEntity` cache + a change-gated prev-morph dispatch; RT
BLAS refits the post-morph deformed buffer (topology fixed), built initially in a representative
resolved-weight pose.

**Node-TRS.** The skin-only import gate (`geometry.cppm:944`) is **lifted** — node-forest import +
animation decode move out and run unconditionally, only the skin payload stays gated; the three channel
skips (weights / non-joint / sparse) are deleted and replaced by real per-channel routing into a
generalized `AnimTrack` (`Target { Bone, Node }` + `Path::Weights` + `jointName→targetName`). One
`sampleClip`/`tickAnimation` drives bones, nodes, and morph weights — the difference is only **where**
the sampled value is written (bone `PoseOverride` / node `PoseOverride` / mesh `MorphWeightOverride`).
Node tracks bind by durable name resolved once to a stable entity `Uuid` (cached, re-resolved on miss).
`localMatrix` already prefers `PoseOverrideComponent` for ANY entity (`scene.cppm:854`), so node-TRS
composes through the existing `updateWorldTransforms` hierarchy with **zero new compose code**. The
minimal parenting foundation (Phase 0) deletes the multi-node flatten-into-world bake and instantiates
a per-node entity forest.

Both surface through the **existing** Timeline (one clip = one bar, channels as nested drill-down) +
Inspector (0..1 weight sliders) + Clips model, driven by new control commands (`set/get-morph-weights`,
`list-clip-bindings`) with node-TRS reusing the single play/seek/loop/state path. The decode helper
uses `cgltf_accessor_unpack_floats` (which applies sparse), **not** `cgltf_accessor_read_float` (which
returns 0 on sparse — a verified blocker the original plan got backwards).

---

## 3. Key decisions (locked, each tied to a real seam)

- **Morph deltas stored SPARSELY** (only moved vertices) as `{ u32 vertexIndex; vec3 dPosition; vec3
  dNormal; }` — 28 B, NORMAL-only on disk. Mirrors UE `FMorphTargetDelta` + Unity's internal sparse
  store + glTF (deltas are commonly sparse accessors); cost scales with affected vertices, not
  vertices×targets, so a 100-shape face rig stays cheap. NORMAL-only because the engine `Vertex`
  (`geometry.cppm:36`) has no tangent stream — a serialized `dTangent` would be dead bytes the deform
  path cannot consume; the tangent is re-derived by Gram-Schmidt against the morphed normal at deform
  time (the UE technique).
  *Rejected:* dense per-vertex-per-target arrays (memory blows up for facial rigs); a 40 B record with
  `dTangent` (dead storage — no tangent stream to consume it); interleaving deltas into the 32 B
  `Vertex` (locked by `static_assert`).

- **Morph applied BEFORE skin**, enforced structurally by the buffer chain: morph writes `morphedBase`,
  which is the skin pass's input binding. The glTF/UE/Unity canonical order (`base + Σ wᵢ·deltaᵢ` →
  renormalize → skin matrix → node/world). Reversing it makes morphs ignore skeletal motion
  (swimming). Making `morphedBase` the skin input means skin-first is **physically impossible** — a
  stronger guarantee than a documented convention — and keeps `skin.slang` non-permuted (a deformed
  batch draws through the static `vertexMain`/`transformVertex` path reading the deformed buffer,
  `inst.model` identity for skinned).
  *Rejected:* morph after skin (wrong space, swimming); a fused single morph+skin kernel (lowest
  bandwidth but awkward with sparse scatter; documented as a perf knob, not v1, to avoid the
  `morphedBase` VRAM round-trip).

- **The morph→skin barrier is graph-derived** by adding an explicit `StorageReadCompute` access to the
  skin pass on `morphedBase`. The skin pass today declares NO access on its input vertex stream on
  purpose (`renderer.cppm:1218-1221`) because that input is host-uploaded/static, not GPU-written. Once
  morph writes it on the GPU, the read access **must** be added or the barrier never derives — the
  exact seam where "the graph derives it" silently fails. Use `StorageReadCompute` (compute-stage
  read), NOT `VertexInputRead` (derives to the wrong pipeline stage). No new `RgUsage` variant needed.
  *Rejected:* relying on the existing pass to derive the barrier without the read access (silently
  absent barrier → sync-validation error); declaring `VertexInputRead` (wrong stage); a hand-written
  barrier (violates the render-graph rule).

- **Format bumps REPLACE the reader.** `.smesh` collapses `MeshFormatVersion(1)` +
  `MeshFormatVersionSkinned(2)` into ONE `MeshFormatVersion` + a flags word (skin/morph bits) + a morph
  section; `.sanim` bumps `AnimFormatVersion` 1→2 with a `Target` kind + `morphCount`; `SceneVersion`
  bumps for `MorphComponent` with a migration branch; `.smodel` is **structurally unchanged** (it
  embeds `.smesh`/`.sanim` chunks verbatim by fourcc TOC). Strongest NO-LEGACY form: the old
  `loadMeshFromBytes` accepting BOTH versions 1 and 2 was itself a mild dual-path, so collapsing to one
  version + flags removes it. Every caller + self-test updated in the same change; v1/v2 files rejected
  with `Err`.
  *Rejected:* keeping the two mesh-version constants and bumping `MeshFormatVersionSkinned` 2→3 (leaves
  the dual-version branch); a separate `.smorph` sidecar (a whole third format+loader+self-test
  surface; folding morph into the embedded `.smesh` chunk is fewer formats); any v1/v2 migration path
  (out of scope per AGENTS.md — start a fresh project).

- **Node tracks bind by durable name resolved ONCE to a stable entity `Uuid`**, cached, re-resolved by
  name on miss; node-TRS reuses the existing `PoseOverrideComponent` (no new component). The UE `FGuid`
  + Unity name-path hybrid the research recommends: a reparent keeps the `Uuid` (binding survives), a
  rename re-resolves by name — avoiding Unity's silent rename/reparent breakage that pure-name-every-
  frame inherits. `localMatrix` already prefers `PoseOverrideComponent` for ANY entity
  (`scene.cppm:854`), so a driven node is just a node with a `PoseOverride` — zero new compose code.
  Unresolved channels warn once and are surfaced by `list-clip-bindings` for editor inspection.
  *Rejected:* pure name-path-every-frame (Unity's documented fragility); a `NodeTrsAnimationComponent`
  (parallel path violating one-code-path); a full rebind-channel mutation (deferred — reimport
  regenerates bindings in this clean-slate engine; only the inspection command ships in v1).

- **Sparse decode uses `cgltf_accessor_unpack_floats`, NOT `cgltf_accessor_read_float`.** Verified
  against `third_party/cgltf/cgltf.h:2355-2357`: `cgltf_accessor_read_float` **returns 0** for a sparse
  accessor (does NOT resolve sparse). `cgltf_accessor_unpack_floats` (`cgltf.h:61,2375`) inits
  from-base-or-zero, applies the sparse overlay, and bounds-checks. The original plan's "read_float
  resolves sparse transparently" was backwards — deleting the `is_sparse` skip while still using
  `read_float` would silently produce all-zero deltas. One `readAccessorDense` helper using
  `unpack_floats` backs both sampler outputs and morph deltas, returning `Err` on a short read.
  *Rejected:* `cgltf_accessor_read_float` for sparse accessors (returns 0 — silent garbage; the
  original plan's load-bearing error); a manual sparse overlay (unnecessary — `unpack_floats` does it
  correctly).

- **Two scene components:** durable `MorphComponent` (serialized, NON_ADDABLE import-managed identity,
  registered in `scene_edit_components.cpp`) + runtime-only `MorphWeightOverrideComponent` (mirrors
  `PoseOverrideComponent`, removed on stop). The override component IS the non-destructive layer — Edit
  preview reverts on stop by construction, with no fragile snapshot/restore (which risks corrupting
  authored weights on crash). `MorphComponent` is import-managed like `SkinnedMesh`, so it goes in
  `NON_ADDABLE`; missing the `registerBuiltinComponents` call means it silently never serializes (the
  easiest catastrophic scene-component miss). Applies to static meshes too (morph needs no rig).
  *Rejected:* writing animated weights directly into the persistent `MorphComponent` with
  snapshot/restore around preview (data-loss-on-crash hazard, dual-state); a single component for both
  authored + animated weights.

- **Control: `set-morph-weights` + `get-morph-weights` + `list-clip-bindings`;** node-TRS REUSES the
  single play/seek/loop/state path; `AnimationClipDto.channels` kind is a plain string, not a `gen.ts`
  enum. A feature adding drivable state gets one `registerCommand` (AGENTS.md). Node-TRS is the same
  `AnimationPlayerComponent`, so duplicating play verbs would violate one-path. `AnimationStateResult`
  gains optional `morphWeights` so the existing `animationVersion`-gated poll carries live values with
  no extra command. `kind` as a string dodges `gen.ts`'s fragile regex enum triple-edit
  (`enumWireNames` + `tsType` + `jsonSchemaFor`). The control-schema contract test + `bun run check`
  gate EVERY control-touching phase, not just the final gate.
  *Rejected:* `play-morph-animation` / `play-node-animation` duplicate verbs (NO LEGACY); a registered
  `AnimChannelKindDto` enum (more typing safety but the fragile triple-edit — a string is lower-risk
  against the regex generator); a `rebind-channel` mutation in v1 (deferred).

- **Timeline/Clips/Inspector EXTENDED, never paralleled:** one clip = one bar; morph targets are
  name-bound scalar weight rows in an Inspector AnimationChannels section; the rig gate widens to one
  `isAnimatable` predicate (`AnimationPlayer || SkinnedMesh || Morph`); canonical 0..1 weight range on
  the wire. The locked UX constraint: the `anim` track row is not multiplied — channels are nested
  drill-down metadata. 0..1 on the wire is a single persisted representation (never 0..100 + 0..1
  dual). Morph rows are scalar float tracks bound by name (UE name-match), reusing the one
  keyframe/curve path. Slider scrubs coalesce through the existing `makeCoalescer` + `dragActive`
  brackets so a drag is one preview per burst.
  *Rejected:* a second timeline or a row-per-channel multiplied lane; a bespoke morph track type; 0..100
  weights (Unity) or storing both representations; per-channel playheads.

- **Phase 0 DELETES the multi-node flatten-into-world bake** (`geometry.cppm:807-825,891`
  `applyNodeTransform`) and instantiates a per-node entity forest; single-node static still collapses
  to one entity. The compose math already exists; the real gap is import/spawn — an unskinned
  multi-node glTF is flattened with `cgltf_node_transform_world` baked into vertices, leaving no live
  local transforms to drive. node-TRS needs the forest (`BoxAnimated`). Deleting (not skipping) the
  flatten path is NO-LEGACY; the highest-regression-risk change, guarded by single/multi/animated
  self-tests.
  *Rejected:* keeping the flatten path alive for unskinned models (double-transform: baked world AND
  live local); a from-scratch parenting build (the math is already there —
  `RelationshipComponent`/`updateWorldTransforms`/`setParent`/`relinkHierarchy`).

---

## 4. Research takeaways (UE5 / Unity / glTF) that shaped the design

- **Sparse delta model.** UE `FMorphTargetDelta` (position + `TangentZ`/normal delta only, tangent
  re-orthonormalized at runtime) and Unity's internal per-shape sparse store (~40 B/affected vertex)
  both key on base-vertex index; cost scales with affected vertices, not vertices×targets. One shared
  read-only delta bank per mesh; per-instance state is just the weight vector + output slice.
- **Normal-only delta + runtime tangent (UE).** Store POSITION + NORMAL; re-derive the tangent by
  Gram-Schmidt against the morphed normal at deform time. The engine `Vertex` has no tangent stream, so
  a serialized tangent delta would be dead storage.
- **Morph-before-skin (glTF spec, UE, Unity).** `base + Σ wᵢ·deltaᵢ` in object space → renormalize the
  normal after the linear sum → skin matrix → node/world. Reversing the order makes morphs ignore
  skeletal motion (swimming). Blend shapes are a compute pre-pass before bone skinning; cost ∝ active
  (nonzero-weight) shapes × moved vertices (skip zero-weight).
- **Active-morph compaction (UE batches; Unity skips zero-weight).** v1 is single-pass float
  accumulation over only nonzero-weight targets; UE's integer-atomic two-pass `InterlockedAdd`
  scatter+normalize is documented as the scale-up path behind a profiling trigger, not v1. We choose a
  0..1 weight convention (vs Unity's 0..100).
- **glTF 2.0 morph + weights stream.** Morph targets are ADDITIVE deltas (POSITION/NORMAL/TANGENT VEC3)
  applied before any transform; weights resolution is `node.weights ?? mesh.weights ?? zeros`; the
  `weights` animation channel output is per-keyframe blocks of N scalars (count = N·M for STEP/LINEAR,
  or 3·N·M for CUBICSPLINE laid `[in[N], value[N], out[N]]` per key); CUBICSPLINE tangents are
  derivatives scaled by `deltaT`; sparse accessors init-from-base-or-zero then overlay
  strictly-increasing indices.
- **Bind by stable id, name as resolver (UE Sequencer / Unity AnimationClip).** UE `FGuid` +
  `FUniversalObjectLocator`; Unity `EditorCurveBinding` name-path. Animate the LOCAL transform, world
  derived from the parent chain; one clip reusable across targets; surface unresolved bindings for
  repair (UE "Fix Actor References").
- **RT BLAS (NVIDIA / UE).** Refit (UPDATE) is valid while topology is fixed (morph never changes it);
  build the initial BLAS in a representative pose, not the zero-weight base; periodic rebuild restores
  traversal quality; budget BLAS updates per frame.
- **Editor UX (UE nested rows + Unity Add Property).** One clip = one timeline bar; T/R/S as grouped
  scalar channels; morph weights as name-bound scalar float rows; a single canonical 0..1 weight range;
  dope-sheet default.

Sources:
[glTF 2.0 — morph targets & animations](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#morph-targets) ·
[UE morph targets](https://docs.unrealengine.com/5.3/en-US/morph-targets-in-unreal-engine/) ·
[Unity blend shapes / SkinnedMeshRenderer](https://docs.unity3d.com/Manual/BlendShapes.html) ·
[NVIDIA RTX best practices (BLAS refit)](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/) ·
[UE Sequencer object bindings](https://docs.unrealengine.com/5.3/en-US/sequencer-overview-in-unreal-engine/)

---

## 5. Phases (dependency-ordered)

| # | Phase | Depends on | One-line |
|---|---|---|---|
| 0 | [Parenting foundation: per-node spawn for unskinned models](phase-0-parenting-foundation.md) | — | delete the multi-node flatten bake; instantiate a per-node entity forest; single/multi/animated self-tests |
| 1 | [`AnimTrack`/`AnimClip` generalization + `.sanim` v2 + sampler](phase-1-track-model-and-sanim.md) | 0 | one track model (Bone/Node/Weights); format replaced; N-wide weights sampler with byte-exact CUBICSPLINE self-test |
| 2 | [Import gate lift + correct sparse decode + node/morph channels](phase-2-import-gate-lift.md) | 1 | one decode path; `cgltf_accessor_unpack_floats` sparse; node-TRS + weights channels + sparse deltas imported |
| 3 | [Morph delta storage + `.smesh` flags collapse + spawn seeding](phase-3-morph-storage-and-smesh.md) | 2 | sparse 28 B morph section; one `MeshFormatVersion`+flags; `MorphComponent` seeded (NON_ADDABLE, registered); `SceneVersion` bump |
| 4 | [Node-TRS runtime (evaluator + spawn players + binding)](phase-4-node-trs-runtime.md) | 1,3 | clips drive node `PoseOverrideComponent`s; name→Uuid cached binding, re-resolve on miss; `clearOverrides` + stop-preview clear |
| 5 | [GPU morph deform stage (compute) + runtime weight application](phase-5-gpu-morph-deform.md) | 3 | `morph.slang` upstream of skin; explicit `StorageReadCompute` access; `MorphWeightOverrideComponent`; per-frame ring active list |
| 6 | [Motion vectors + RT BLAS for morphed geometry](phase-6-motion-vectors-and-rt.md) | 5 | prev-weights cache; change-gated prev dispatch; representative-pose BLAS build; object-space unskinned-morph TLAS |
| 7 | [Control plane: morph + binding commands + channel metadata](phase-7-control-plane.md) | 4,5 | `set/get-morph-weights`, `list-clip-bindings`; clip channel descriptors; contract test gates the phase |
| 8 | [Frontend: Timeline channel drill-down + Inspector morph sliders](phase-8-frontend-timeline-inspector.md) | 7 | extend Timeline/Clips/Inspector; one `isAnimatable` gate; 0..1 sliders; coalesced scrubs |
| 9 | [Docs + e2e fixtures + perf budget + milestone gate](phase-9-docs-and-e2e.md) | 2-8 | docs pages + hub rows; `BoxAnimated`/`AnimatedMorphCube`/`MorphStressTest` e2e; deformation budget + two-pass graduation; final `make check` |

Dependency rationale: the spawn/parenting cutover (0) unblocks node-TRS and the per-node morph mesh
entity; the track model + `.sanim` + sampler (1) precede the importer (2) so writer/reader exist before
decode; morph storage + `.smesh` (3) needs the importer (2); node-TRS runtime (4) needs the data model
(1) and spawn (3); the GPU stage (5) needs storage (3); motion/RT (6) needs the deform stage (5);
control (7) needs the runtime state (4,5); frontend (8) needs the control DTOs (7); docs + e2e + budget
(9) close it out.

---

## 6. Keep-current (part of "done", per AGENTS.md)

- After **each** phase: `make engine` then `make prepare-for-commit` (format + lint); fix every warning
  the phase raises. A format bump extends its matching self-test (`runSceneSerializationSelfTest`, the
  `.smesh`/`.sanim` round-trip self-tests, `runAnimationSelfTest`) in the **same** phase.
- Each drivable-state phase (5,7) adds its `registerCommand` and runs the control-schema contract test
  + `bun run check` (git-diff-clean on the generated files); each concept phase (2,3,4,5,6) updates the
  matching `docs/content/explanations/animation/` page + its hub `_index.md` row in the same change.
- NO LEGACY enforced per phase: a format bump deletes the old reader; the gate lift deletes the
  skin-only animation path; the flatten-into-world bake is deleted for multi-node unskinned models;
  `jointName`→`targetName` updates every caller in the same change.
- **Concurrent builds:** use a private `build/<name>` dir while iterating the new `morph.slang` compute
  shader if another agent is building (the `.pcm` mmap race causes spurious Bus errors).
