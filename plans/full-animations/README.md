# Full animation — morph targets + node-TRS animation — design

**Status:** NOT STARTED

The decision-locked design for two animation capabilities, built on Saffron's existing seams with the
smallest correct cutover (conventions-purist / minimal-footprint), incorporating the two correctness
fixes the review surfaced (the cgltf sparse-decode primitive and the N-wide CUBICSPLINE weights layout)
and the best ideas from the performance/AAA-parity and editor-UX angles.

1. **Morph targets (blend shapes)** — full-fidelity POSITION+NORMAL deltas (TANGENT reconstructed at
   deform time, the UE/glTF approach), sparse-accessor decode, all three interpolation modes (incl.
   CUBICSPLINE on the weights stream), arbitrary N targets, a GPU morph stage composed *before* the
   existing compute-skinning prepass, motion vectors + RT BLAS update carried through, a scene
   component, control commands, and the editor Timeline/Clips/Inspector drill-down.
2. **Node-TRS animation (non-skeletal)** — animate plain entity `TransformComponent`s from glTF
   translation/rotation/scale channels bound to nodes. The skin-only import gate
   (`geometry.cppm:944`) is **lifted**, not duplicated: one decode path produces both joint tracks and
   node tracks. Bindings resolve by durable node name to a stable entity Uuid (resolved once, cached,
   re-resolved on miss), and a control command surfaces resolved/unresolved status for editor repair.

Both ride a **minimal scene-graph parenting / transform-hierarchy** foundation (Phase 0) — the
local/world compose math already exists (`relinkHierarchy`, `setParent`, `updateWorldTransforms`,
`localMatrix` prefers `PoseOverrideComponent`); the real gap Phase 0 closes is import/spawn: an
unskinned multi-node glTF is today **flattened** (`cgltf_node_transform_world` baked into vertices at
`geometry.cppm:891`, `applyNodeTransform=true`), so node-TRS has no live local transforms to drive.
Phase 0 deletes that flatten path for multi-node unskinned models and instantiates a per-node entity
forest instead.

This is **a plan only.** No engine/editor source is touched here. Each `phase-N-*.md` carries its own
`**Status:**` line and the concrete edits; this README is the locked design they implement.

---

## 1. North star + principles (locked)

**Target.** `BoxAnimated.gltf` (node-TRS, hierarchy, no skin) imports with its animation intact and
plays back driving plain entity transforms through the existing hierarchy. `AnimatedMorphCube.gltf`
(2 targets, CUBICSPLINE weights animation) and `MorphStressTest`/`MorphPrimitivesTest` (sparse,
multi-target) import their morph deltas + weights animation; the GPU morph stage feeds the skin
prepass (or feeds the deformed buffer directly for an unskinned morph mesh); morphs are
scriptable/inspectable over the control plane; the editor Timeline shows the clip with per-channel
(bone / morph / node) drill-down in the Inspector — all inside the **existing** Timeline + Clips +
Inspector model, no parallel UI.

**Principles (locked), each tied to a real seam:**

1. **Generalize, never parallel.** `AnimTrack` / `AnimClip` / `AnimationPlayerComponent` are extended,
   not cloned. `AnimTrack` gets a `Target { Bone, Node }` kind, a `Path::Weights` variant, and a
   `targetName` that subsumes today's `jointName`. There is exactly **one** `sampleTrack` /
   `sampleClip` / `tickAnimation` path; the bone-vs-node-vs-morph difference is *where the sampled
   value is written*, not a second evaluator. (`geometry.cppm:79-110`; `animation.cppm:80-123`;
   `animation.cpp:603-764`.)

2. **One import decode, gate lifted.** The `if (data->skins_count > 0 && sawSkinnedPrimitive &&
   !sawUnskinnedPrimitive)` block (`geometry.cppm:944`) is split: node-forest import and animation
   decode move **out** of the skin gate and run unconditionally; only the skin payload stays gated.
   The `weights` / non-joint / `is_sparse` skips (`geometry.cppm:1043,1059,1066`) are **deleted** and
   replaced by real decode. No skinned-only path lingers (NO LEGACY).

3. **Sparse decode via the correct cgltf primitive.** `cgltf_accessor_read_float` returns 0 for
   sparse accessors (`cgltf.h:2357`) — it does NOT resolve sparse. The decode helper uses
   `cgltf_accessor_unpack_floats` (`cgltf.h:61,2375`), which initializes from the base bufferView (or
   zeros when absent), applies the sparse overlay, and validates internally. The importer asserts the
   returned count matches the expected element count and fails with `Err(...)` on mismatch — never an
   exception. This applies to morph-delta accessors and animation sampler outputs alike.

4. **Morph-before-skin, one prepass family.** The morph accumulation is a compute stage that runs
   *immediately upstream* of the existing skin prepass and writes the same 32-byte deformed `Vertex`
   layout that skin consumes, so every downstream pass (depth, shadow, gbuffer, scene, motion, RT
   BLAS) reads it unchanged — the glTF/UE/Unity canonical order (`skin.slang`;
   `renderer.cppm:1215-1275`). The order is enforced *structurally*: the morph stage writes
   `morphedBase`, which IS the skin pass's input binding, so skin-first is physically impossible. For
   an *unskinned* morph mesh the morph stage writes the deformed buffer directly and the mesh draws
   from it as a skinned-style static stream (one deformed-buffer contract, no second deform path).

5. **Render graph derives every barrier.** The morph compute pass declares `StorageWriteCompute` on
   its output; the skin pass adds an explicit `StorageReadCompute` access on `morphedBase` (its input
   is GPU-written now, unlike the host-uploaded static stream that needed no access — this added read
   access is the seam where "the graph derives it" would otherwise silently fail). The morph→skin and
   skin→consumer barriers are derived, never hand-written (`render_graph.cppm`; `renderer.cppm:1235`).

6. **Reuse the blend layer, two override payloads.** Morph **weights** live in a durable per-entity
   `MorphComponent` array seeded from `mesh.weights` (overridable by `node.weights`); the animated
   weights the evaluator writes each frame go into a runtime-only `MorphWeightOverrideComponent`
   (mirrors `PoseOverrideComponent`, removed on stop → reverts to authored weights, never serialized
   → Edit preview stays non-destructive *by construction*, no snapshot/restore). Node-TRS reuses the
   existing `PoseOverrideComponent` (`scene.cppm:123-128`) on the node entity — the same runtime-only
   override `localMatrix` already prefers (`scene.cppm:852-860`), so node-TRS composes through the
   hierarchy with zero new compose code.

7. **Format replacement, never dual-path.** `.smesh` collapses its two version constants
   (`MeshFormatVersion=1`, `MeshFormatVersionSkinned=2`, whose loader accepts both — itself a mild
   dual-path) into ONE `MeshFormatVersion` + a feature-flags word in `SMeshHeader` (skin bit, morph
   bit), and gains a sparse morph section; the old dual-version branch is deleted. `.sanim` gains the
   `Weights` path + node-vs-bone target kind → bump `AnimFormatVersion`, old reader deleted.
   `SceneVersion` bumps for the new `MorphComponent` with a migration branch. The `.smodel` container
   embeds `.smesh`/`.sanim` chunks **verbatim** by fourcc TOC, so its framing version
   (`ContainerFormatVersion`) is unchanged — only the embedded chunk contents evolve. Every caller +
   every self-test is updated in the **same** change (`geometry.cppm:136-139,430`; serde self-tests).

8. **State worth driving gets a control command; concepts get docs.** New per-entity state (morph
   weights, node-TRS playback, binding inspection) gets a `registerCommand` in
   `control_commands_animation.cpp`; node-TRS reuses the single `play-animation`/`seek`/`loop`/`state`
   path (no duplicate playback verb). Each concept gets a `docs/content/explanations/animation/` page
   + hub row in the same change (AGENTS.md keep-current). The control-schema contract test +
   `bun run check` (git-diff-clean on the 5 generated files) gate **every** control-touching phase,
   not only the final gate.

9. **`std::expected`, module DAG, no exceptions** throughout — the importer fails sparse/validation
   violations with `Err(...)`, never throws; `Saffron.Animation` keeps importing only
   `Geometry`+`Scene`; the morph GPU stage lives in `Saffron.Rendering`; the Lua `sa.setMorphWeight`
   binding is a host-bound `std::function` over a POD (length-prefixed array) bridge, so
   `Saffron.Script` never imports `Saffron.Animation`.

---

## 2. Research takeaways (UE5 / Unity / glTF) that shaped the design

- **Sparse delta model (UE `FMorphTargetDelta`, Unity internal sparse store).** Store only moved
  vertices keyed by base-vertex index; cost scales with affected vertices, not vertices×targets. Drop
  sub-threshold deltas at import. One shared read-only delta bank per mesh across all instances;
  per-instance state is just the weight vector + output slice.
- **Normal-only delta + runtime tangent (UE).** Store POSITION + NORMAL deltas; re-derive the tangent
  by Gram-Schmidt against the morphed normal at deform time. The engine `Vertex` has no tangent stream
  (tangent is a material-time derivation), so a serialized tangent delta would be dead storage — we do
  not store one.
- **Morph-before-skin (glTF spec, UE, Unity).** `base + Σ wᵢ·deltaᵢ` in object space → renormalize
  normal → skin matrix → node/world. Reversing the order makes morphs ignore skeletal motion
  (swimming).
- **Active-morph compaction (UE batches 128, Unity skips zero-weight).** v1 is single-pass float
  accumulation over only the nonzero-weight targets; the UE integer-fixed-point two-pass
  `InterlockedAdd` scatter+normalize is documented as the scale-up path behind a profiling trigger,
  not v1.
- **Motion vectors must include morph (UE/Unity).** A morph-only motion (blink, lip) gets zero
  velocity and ghosts under TAA unless the previous-frame deform uses the previous frame's weights.
  Double-buffer the deform output and cache per-entity previous weights; skip the prev-morph dispatch
  when weights are unchanged.
- **RT BLAS refit + periodic rebuild (NVIDIA/UE RT guidance).** Refit (UPDATE) is valid while topology
  is fixed (morph never changes it); build the initial BLAS in a representative resolved-weight pose
  (not the zero-weight base) so refits stay near the build's spatial assumptions; a periodic full
  rebuild cadence is the documented knob to restore traversal quality.
- **glTF weights stream layout.** The `weights` sampler output is per-keyframe blocks of N scalars
  laid end-to-end (count = N·M for STEP/LINEAR, 3·N·M for CUBICSPLINE as `[in[N], value[N], out[N]]`
  per keyframe). CUBICSPLINE tangents are derivatives and MUST be scaled by `deltaT = t_{k+1}-t_k`.
  The first in-tangent and last out-tangent are present but unused.
- **Bind by stable id, name as resolver (UE FGuid + Unity name-path).** Resolve a node track's
  `targetName` to a stable entity Uuid once at clip-load, cache it; re-resolve by name on miss; surface
  unresolved channels for repair. Avoids Unity's silent rename/reparent breakage.
- **Local TRS animated, world derived (both engines).** Channels target local translation/rotation/
  scale; world falls out of the parent chain. Never animate world directly.
- **Timeline = one clip, one bar; channels nested (UE nested rows, Unity Add Property).** Morph-weight
  rows are name-bound scalar float tracks, not a bespoke track type; T/R/S are grouped scalar channels.
  Canonical weight range 0..1 on the wire (display % if desired); one persisted representation.

---

## 3. The data models (locked shapes)

### 3.1 `AnimTrack` generalization (replaces the bone-only shape)

`AnimTrack` (`geometry.cppm:79-101`) gains:

- `enum class Target : u8 { Bone, Node }` — what the track drives. `Bone` indexes
  `SkinnedMeshComponent.bones`; `Node` binds by `targetName` (the durable node name) to a scene entity.
- `Path` gains `Weights` (the morph-weights channel). For `Weights`, `values` is the flat per-keyframe
  block of N target scalars (CUBICSPLINE = 3·N per key, laid `[in[N], value[N], out[N]]`),
  `morphCount` records N.
- `jointName` is **renamed** `targetName` (durable binding key for both bone and node) in one atomic
  change touching `sampleClipResolved`, the `.sanim` serde, and the control DTO.
- `i32 joint` stays (bone index; -1 for node/weights tracks).

`AnimClip` is unchanged in shape; it now legitimately holds bone, node, and weights tracks side by
side — one clip, mixed channels, exactly as glTF authors them. A fully-mixed glTF (skinned + morph +
node tracks in one animation) attaches **one** `AnimationPlayerComponent` to the model root, which
writes: bone `PoseOverrideComponent`s, node `PoseOverrideComponent`s, and the mesh entity's
`MorphWeightOverrideComponent` — three write targets, one evaluator pass.

### 3.2 Morph delta storage (sparse, normal-only)

```
struct MorphDelta   { u32 vertexIndex; glm::vec3 dPosition; glm::vec3 dNormal; };  // 28 B, tightly packed
struct MorphTarget  { std::string name; std::vector<MorphDelta> deltas; };          // sparse: only moved verts
```

Sparse by construction. No `dTangent` field (no tangent stream to consume; tangent re-derived at deform
time). Sub-threshold deltas dropped at import. The merged `Mesh` concatenates primitives into submeshes
with a `vertexOffset`; morph delta `vertexIndex` is offset by the submesh `vertexOffset` so one bank
indexes the merged vertex stream. Import validates: every target accessor `count` == base POSITION
`count`; all of a node's primitives agree on target count; a `weights` channel never targets a
zero-target mesh.

### 3.3 New / changed scene components

- `MorphComponent` (new, `scene.cppm`) — on the **mesh** entity beside `SkinnedMeshComponent` /
  `MeshComponent`: `std::vector<f32> weights` (resolved per-target weights, seeded from
  `node.weights ?? mesh.weights ?? zeros`). Serialized (durable defaults). Import-managed identity, so
  it is in `NON_ADDABLE` in the Inspector and registered in
  `scene_edit_components.cpp registerBuiltinComponents` (missing the registration means it silently
  never serializes). Applies to static meshes too (morph does not require a rig).
- `MorphWeightOverrideComponent` (new, runtime-only, mirrors `PoseOverrideComponent`) — the *animated*
  weights the evaluator writes each frame; the deform stage reads this if present, else
  `MorphComponent.weights`. Never serialized; removed when playback stops (and by `stop-preview`),
  reverting to authored weights.
- `AnimationPlayerComponent` (extended in behavior only, `scene.cppm:92-117`) — no new component for
  node-TRS. A player that targets a non-rigged subtree drives nodes; existing fields all apply.
- **No** `NodeTrsAnimationComponent` (node-TRS reuses `PoseOverrideComponent`).

---

## 4. Where each concern hooks (the seam map)

| Concern | File · symbol | Change |
|---|---|---|
| Parenting: per-node spawn for unskinned models | `assets.cppm` `spawnModel`/`spawnSkinnedModel` (`:4818,:4972`), `geometry.cppm` `appendPrimitive` (`:748,:807-825,:891`) | delete multi-node flatten; instantiate a node forest; single-node static still collapses |
| Morph delta + weights decode, gate lift | `geometry.cppm` `importGltfModel` (`:944`, `:1043-1106`) | lift node/anim out of skin gate; decode `targets`, `weights`, sparse accessors |
| Sparse accessor decode | `geometry.cppm` (new helper) | `cgltf_accessor_unpack_floats`; verify count; `Err` on mismatch/zero-target weights |
| `.smesh` flags + morph section | `geometry.cppm` `encodeMeshImage`/`loadMeshFromBytes`/`loadMeshSkinFromBytes` (`:1401,:1489,:1589`), `SMeshHeader` (`:386`) | collapse to one `MeshFormatVersion` + flags; append morph section; delete dual-version branch |
| `.sanim` weights/node tracks | `geometry.cppm` `saveAnimationToBuffer`/`loadAnimationFromBytes` (`:1619,:1657`), `SANimTrackRecord` (`:418`) | bump `AnimFormatVersion`; add target-kind + morphCount; rename to `targetName`; replace reader |
| Sampler (weights + node) | `animation.cpp` `sampleTrack`/`sampleClip`/`sampleClipResolved` | one mode-keyed evaluator; N-wide weights loop with 3·N CUBICSPLINE offsets; node write path |
| Node + morph application | `animation.cpp` `tickAnimation` (`:603-764`) | write `PoseOverrideComponent` to nodes; `MorphWeightOverrideComponent` to mesh; clear on stop |
| Hierarchy compose | `scene.cppm` `localMatrix`/`updateWorldTransforms` (`:852,:914`) | already prefers `PoseOverride`; node-TRS rides it for free |
| GPU morph stage | `renderer.cppm` (new pass before skin, `:1215`), `morph.slang` (new) | `StorageWriteCompute` morph output; skin pass adds `StorageReadCompute` on `morphedBase` |
| Morph dispatch + motion | `renderer_drawlist.cpp` `submitDrawList` (`:434,:645,:791`), `renderer_types.cppm` `Skinning`/`SkinDispatch` (`:78,:620,:1206`) | per-instance morph dispatch (host-mapped ring) + `prevMorphWeightsByEntity`; change-gated prev dispatch |
| Spawn seed weights/players | `assets.cppm` `spawnSkinnedModel`/`spawnModel` (`:4818,:4972`) | seed `MorphComponent`; upload delta bank; attach a node-TRS player to non-rigged animated subtrees |
| Control: morph + bindings | `control_dto.cppm` (`:1457+`), `control_commands_animation.cpp` (`:208`) | `set-morph-weight`, `get-morph-weights`, `list-clip-bindings`; extend `AnimationClipDto`/`AnimationStateResult` |
| Frontend Timeline/Inspector | `shared.ts`, `TimelinePanel.tsx`, `TimelineSurface.tsx`, `InspectorPanel.tsx`, `SkeletonTree.tsx`, `store.ts` | channel drill-down + 0..1 morph sliders; widen the rig gate |
| Docs | `docs/content/explanations/animation/` + `geometry-and-assets/` hubs | morph-targets, node-trs pages; bump format pages |

---

## 5. Phases (dependency-ordered)

| # | Phase | Depends on | One-line |
|---|---|---|---|
| 0 | Parenting foundation: per-node spawn for unskinned models | — | delete the flatten-into-world path; instantiate a node forest; single/multi/animated self-tests |
| 1 | `AnimTrack`/`AnimClip` generalization + `.sanim` v2 + sampler | 0 | one track model (Bone/Node/Weights); format replaced; N-wide weights sampler with byte-exact CUBICSPLINE self-test |
| 2 | Import gate lift + correct sparse decode + node/morph channels | 1 | one decode path; `unpack_floats` sparse; node-TRS + weights channels + sparse deltas imported |
| 3 | Morph delta storage + `.smesh` flags collapse + spawn seeding | 2 | sparse morph section; one `MeshFormatVersion`+flags; `MorphComponent` seeded; `SceneVersion` bump |
| 4 | Node-TRS runtime (evaluator + spawn players + binding) | 1,3 | clips drive node `PoseOverrideComponent`s; name→Uuid cached binding; re-resolve on miss |
| 5 | GPU morph deform stage (compute) + runtime weight application | 3 | morph-before-skin compute pass; explicit skin read access; `MorphWeightOverrideComponent` |
| 6 | Motion vectors + RT BLAS for morphed geometry | 5 | prev-weights cache; change-gated prev dispatch; representative-pose BLAS build; object-space unskinned-morph TLAS |
| 7 | Control plane: morph + binding commands + channel metadata | 4,5 | `set/get-morph-weight`, `list-clip-bindings`; clip channel descriptors; contract test gates the phase |
| 8 | Frontend: Timeline channel drill-down + Inspector morph sliders | 7 | extend Timeline/Clips/Inspector; widen the rig gate; 0..1 sliders; coalesced scrubs |
| 9 | Docs + e2e fixtures + perf budget + milestone gate | 2-8 | docs pages + hub rows; `BoxAnimated`/`AnimatedMorphCube`/`MorphStressTest` e2e; deformation budget; final `make check` |

Dependency rationale: the spawn/parenting cutover (0) unblocks node-TRS and the per-node morph mesh
entity; the track model + `.sanim` + sampler (1) precede the importer (2) so the writer/reader exist
before decode; morph storage + `.smesh` (3) needs the importer (2); node-TRS runtime (4) needs the data
model (1) and spawn (3); the GPU stage (5) needs storage (3); motion/RT (6) needs the deform stage (5);
control (7) needs the runtime state (4,5); frontend (8) needs the control DTOs (7); docs+e2e+budget (9)
close it out. Each phase ends at a green `make engine` + `make prepare-for-commit`, with format/serde
self-tests extended in the same phase that bumps a format, and the control-schema contract test run at
every control-touching phase.

---

## 6. Keep-current (part of "done", per AGENTS.md)

- After **each** phase: `make engine` then `make prepare-for-commit` (format + lint), fix every warning
  the phase raises. Format bumps extend the matching self-test (`runSceneSerializationSelfTest`, the
  `.smesh`/`.sanim` round-trip self-tests, `runAnimationSelfTest`) in the same phase.
- Each drivable-state phase (5,7) adds its `registerCommand` and runs the contract test +
  `bun run check`; each concept phase (2,3,4,5,6) updates the matching `docs/` page + hub row.
- NO LEGACY is enforced per phase: a format bump deletes the old reader; the gate lift deletes the
  skin-only animation path; the flatten-into-world path is deleted for multi-node unskinned models;
  `jointName`→`targetName` updates every caller in the same change.
- **Concurrent builds:** use a private `build/<name>` dir while iterating the new `morph.slang` compute
  shader if another agent is building (the `.pcm` mmap race causes spurious Bus errors).
