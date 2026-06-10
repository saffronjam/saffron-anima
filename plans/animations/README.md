# skeletal animation plan

This plan brings **skeletal animation** to the engine, end to end: import animation clips from glTF,
**play** them (loop / once / ping-pong, speed, blend transitions) so a rigged mesh deforms, draw a
**line-skeleton overlay** in the viewport, make skinned characters **render correctly in every pass**
(TAA, shadows, AO, ray tracing — today they only draw in the main scene pass), expose a polished
**read-only timeline** in a new bottom dock of the editor, and lay the **architecture for physics-driven,
two-way animation** (kinematic foot IK now; powered ragdoll when Jolt lands) without a later rewrite.

The reference points throughout are **UE5** (`USkeleton`/`USkeletalMesh`/`UAnimSequence`, Physical
Animation Component + Physics Blend Weight, Sequencer's three-region timeline) and **Unity** (Avatar /
AnimationClip / Animator, the deliberate read-only-Timeline-vs-keyframe-Animation-window split), with
**glTF 2.0** as the on-the-wire data model and **Frostbite/Ubisoft motion matching** as the named
(deferred) destination for environment-reactive locomotion.

## Why this is mostly extension, not new machinery

The engine **already skins**. This is not a from-zero feature; the import + GPU skinning halves exist and
are correct — the gaps are *clips, a runtime, an overlay, editor UI, and multi-pass correctness*.

What already works (verified against the code):

- **glTF skins import.** `importGltfModel` (`geometry.cppm:383`) parses `JOINTS_0`/`WEIGHTS_0` into a
  parallel `VertexSkin` stream (`geometry.cppm:58` — `u16vec4 joints` + `vec4 weights`, 4 influences,
  24 B), reads the skin (`ImportedSkin` `geometry.cppm:79` — `joints`, `inverseBind`, `skeletonRoot`,
  `meshNode`) and the node forest (`ImportedNode` `geometry.cppm:67`). `.smesh v2`
  (`MeshFormatVersionSkinned = 2`, `geometry.cppm:90`) appends the skin stream after submeshes.
- **Bones are real entities.** `spawnSkinnedModel` (`assets.cppm:992`) makes one entity per node, tags
  joints with `BoneComponent` (`scene.cppm:67`), and attaches a `SkinnedMeshComponent` (`scene.cppm:78`
  — `mesh`, `rootBone`, `bones[]` (Uuid, joint order), `inverseBind[]`, `boneHandles[]` cache).
- **Vertex-shader skinning runs.** Each frame `jointMatrices` (`scene.cppm:650`) builds the
  `worldMatrix(bone)·inverseBind` palette, uploaded as a per-frame SSBO at **set 2, binding 1**
  (`mesh.slang:151`); `vertexMainSkinned` (`mesh.slang:325`) does 4-influence linear-blend skinning and
  **correctly ignores the mesh-node transform** (the #1 glTF skinning bug) — there is even a self-test.

What is missing — and what this plan builds:

1. **Clips.** `cgltf` parses `data->animations[]` but `importGltfModel` **never reads it**
   (`geometry.cppm:383-620`). Keyframes are silently dropped. There is no clip data type, no sampler,
   no `AssetType::Animation`.
2. **A runtime.** No `AnimationPlayerComponent`, no pose sampling, no playback state, no blend layer.
3. **A skeleton overlay.** None exists.
4. **A timeline / animation viewer.** No bottom dock exists at all (`Layout.tsx:176-191` is a 2-panel
   viewport/assets split).
5. **Multi-pass skinning correctness.** Skinned batches `continue` out of the depth prepass
   (`renderer_drawlist.cpp:567`), shadows (`:594`, `:761`), the SSAO G-buffer (`:625`), and **motion
   vectors** (`:657`); the BLAS is built from the static bind pose (`renderer_detail.cppm:490`). So
   animated characters **ghost under TAA, cast/receive no shadows or AO, and don't deform in any
   ray-traced effect** — a latent defect today, fixed here by a compute skinning pre-pass.

## The one load-bearing decision

**The pose already lives in the scene graph.** `renderScene` runs `updateWorldTransforms`
(`scene.cppm:613`) → `jointMatrices` (`scene.cppm:650`), which compose each bone's world matrix and read
it through the cached `WorldTransformComponent`. So playback needs no change to rendering, the render
graph, or the palette upload — only a new *source* for each bone's local transform when it is being
animated. That source is a runtime **`PoseBuffer`** (decomposed local TRS per joint, runtime-only like
`WorldTransformComponent`); the bone `TransformComponent`s are left holding the **rest pose**, untouched.
Because the authored transforms are never overwritten, animation is **non-destructive in both Edit and
Play** — no scene dirtying, no snapshot/restore — which is exactly what an animation viewer/timeline that
scrubs clips in Edit mode requires (UE5 Persona/Sequencer and Unity's Animation window both preview
animation **without** entering play).

The decision that shapes everything after is **how the sampled pose reaches the bones**. We route it
through that `PoseBuffer` and an **inert per-bone blend layer** (`out[i] = blend(animated[i], override[i],
weight[i])`, slerp on rotations), and `updateWorldTransforms` composes bone world matrices from the result
— never by overwriting the rest-pose `TransformComponent`s. In v1 every `weight[i] == 0`, so the blend
layer does nothing — but it is the seam every later producer plugs into: a foot-IK solver (Phase 13) and,
when Jolt arrives, a powered ragdoll both become *just another override-pose producer* writing
`override[i]` + `weight[i]`, with the sampling graph untouched. This is UE5's **Physics Blend Weight**
model. Overwriting bone transforms directly would ship faster but dirty the scene in Edit mode and force a
rewrite the first time physics needs to bend the result — so the PoseBuffer + (inert) blend layer is the
v1 design, not a later upgrade.

## What "done" looks like

- A new **`Saffron.Animation`** module (DAG edge `Animation → {Core, Geometry, Scene}`) owning the pose
  representation, clip sampling (all 3 glTF interpolation modes, slerp rotations), looping/speed,
  transitions, the per-bone blend layer, and the per-entity evaluator.
- glTF animation clips import to sidecar **`.sanim`** files, one **`AssetType::Animation`** catalog entry
  per clip, bound to bones **by stable joint index + name** (so a clip survives reload and, later,
  retargeting). The skeleton stays **implicit** for v1 (the bone-entity subtree + `SkinnedMeshComponent`);
  `AssetType::Skeleton` is deferred.
- A dumb-data **`AnimationPlayerComponent`** on `Saffron.Scene` (clip Uuid, time, speed, wrap mode,
  playing, transition state) drives a rigged entity; the Host evaluates it in **both Edit (preview) and
  Play** — during Play just before the `simTick` seam (so a script can still override a bone), and in Edit
  when the timeline previews/scrubs the selected entity.
- **Looping / once / ping-pong + speed + cross-fade (inertialized) transitions** all work; a clip plays
  and the mesh deforms — **previewed and scrubbed in Edit** (the timeline's whole purpose) and played as
  part of the simulation during **Play**.
- Skinned meshes are deformed once by a **compute pre-pass** into a buffer every pass reuses, so TAA,
  shadows, SSAO, and ray tracing are all correct for characters (per-instance previous-frame motion via a
  double-buffered palette + `prevModel`).
- A **line-skeleton overlay** (bone segments + joint dots + optional RGB axis lines) draws on top of the
  mesh for the selected entity — in **both Edit and Play** — reusing the native overlay path, with a
  `show-bones` control toggle.
- A new **full-width resizable bottom dock** (tab system copied from the right "tools" sidebar) hosts a
  **read-only `TimelinePanel`**: track rows / ms ruler / clip bars / scrubbable playhead / transport +
  Loop / `Duration · N tracks · N clips` footer, canvas-rendered like `FrameTimeGraph`, reading playback
  state over the control plane.
- **Kinematic foot IK** (two-bone analytic solver + ground hook) runs as the first real blend-layer
  override producer — the proof that the physics-ahead architecture holds — and per-bone physics metadata
  fields are reserved on the skeleton for the eventual Jolt build.
- Scriptable from `se`, covered by `tests/e2e/animation.test.ts`, documented under `docs/content/`.

## Design decisions (locked)

- **Clips are sidecar `.sanim` files, not embedded in `.smesh`.** Magic `SANM`, one file + one
  `AssetType::Animation` catalog entry per glTF animation. Rationale: clips must be addressable and
  reusable independently of any one mesh (UE5/Unity both make them standalone assets); embedding would
  re-bake geometry on every clip edit. `.smesh` keeps its clean v1/v2 header. The per-vertex `VertexSkin`
  stream stays embedded (it is intrinsically per-mesh) and **unchanged** — 4 influences, 24 B, spec-correct.
- **Skeleton is implicit for v1.** Clips target bones by **stable joint index + name** into
  `SkinnedMeshComponent.bones`, never by entt handle (handles are a post-load cache). A standalone
  `AssetType::Skeleton` (the cross-mesh-reuse / retargeting enabler) is **deferred** but the by-name
  binding keeps the door open.
- **New module `Saffron.Animation`, edge `→ {Core, Geometry, Scene}`, between Scene and Assets.** It
  manipulates `glm` types directly (a heavy C++ header), so — like `scene`/`geometry` — it uses classic
  `#include` in the global module fragment and does **NOT** `import std`. The runtime clip type + `.sanim`
  file IO live in **`Saffron.Geometry`** (it owns mesh/file formats); `Saffron.Animation` consumes them.
- **Pose flow: sample → `PoseBuffer` (local TRS) → inert per-bone blend layer → world-transform
  composition.** The blended pose feeds `updateWorldTransforms`/`jointMatrices`; bone `TransformComponent`s
  keep the **rest pose** and are never overwritten, so animation is non-destructive (no scene dirtying) and
  identical in Edit and Play. No new rendering path for playback. `AnimationPlayerComponent` is dumb data in
  `Saffron.Scene` (zero Animation knowledge), mirroring how `ScriptComponent` carries no Lua.
- **Evaluation runs in Edit (preview) and Play — decoupled from the game's play state**, mirroring UE5
  (Persona preview / in-editor component tick / Sequencer scrub) and Unity (Animation window + Timeline
  non-destructive sampling); neither gates *viewing* an animation on entering play. The Host runs
  `tickAnimation` each frame **before** `tickPlay`, so during Play animation lands before scripts (the
  `simTick` seam, `scene_edit_context.cppm:211` / `scene_edit_play.cpp:199`) and a script can still
  override a bone. During **Play** all rigs with `playing` advance (the simulation); during **Edit** only
  the **previewed** entity advances (driven by the timeline transport / a control command). A `seek` just
  sets `time`; the always-running evaluator re-samples it next frame — no special paused-seek path. It is
  non-destructive in Edit because the pose lands in the runtime `PoseBuffer`, not the rest-pose transforms.
- **Skinning execution moves to a compute pre-pass** writing a deformed-vertex buffer in the base 32-B
  `Vertex` layout that every pass reads on binding 0 — collapsing the per-pass skinned-shader permutations
  to zero and feeding the BLAS. Until graph-created transient resources exist (Status: not built), the
  deformed buffer is allocated **grow-only per frame-in-flight**, like `Instancing.jointBuffers`
  (`renderer_types.cppm:1065`). LBS now; a dual-quaternion path is a later per-mesh kernel flag.
- **Skeleton overlay reuses the native overlay** (`addLine`/`addCircleFill`, `host.cppm`), drawn **on top**
  (the overlay PSO is `depthTestEnable = VK_FALSE`, `renderer_pipelines.cpp:230`). It must render in
  **Play too** — the current `submitNativeGizmo` call is gated to `PlayState::Edit` (`host.cppm:683`), so
  the skeleton submit is lifted out of that guard while the manipulation gizmo stays Edit-only.
- **Timeline v1 is a read-only viewer**, not a keyframe editor (Unity's Timeline-vs-Animation-window
  split). It reads `get-animation-state` on the existing reconcile poll, gated on a new **`animationVersion`**
  stamp added to `PlayStateResult`/`SelectionResult`; the playhead scrubs through `useScrubValue` +
  `makeCoalescer` → `seek-animation`. Canvas-rendered (imperative `setData` on rAF, never per-tick React
  state — the `FrameTimeGraph` rule, which the recent render-frequency commits exist to protect).
- **Bottom dock copies the right "tools" model** (`RightSidebar.tsx`: a `BottomTool` union,
  `bottomTools`/`activeBottomTool` slices, open/close, vanishes when empty), **not** the always-present
  left Radix-Tabs. Opened from a `Topbar` button; height persisted like `rightSidebarWidth`; resize/open/
  close fire `emitLayoutSettled({force:true})` so `ViewportPanel` re-glues the Wayland subsurface
  (`ViewportPanel.tsx:239`).

## Explicitly OUT (deferred)

A standalone `AssetType::Skeleton` + cross-mesh clip reuse + retargeting; keyframe
**authoring** in the timeline (diamonds/curves — the lane renderer is built to accept them later);
per-channel/per-bone track rows (v1 = one clip bar per bound entity); animation state machines / blend
trees / additive layers / avatar masks; **powered/passive ragdoll**, hit reactions, `SkeletonMapper`
(all wait for Jolt — the per-bone metadata reserved in Phase 13 is what makes that build mechanical);
motion warping / pose warping; **motion matching** & learned motion matching; dual-quaternion skinning;
morph-target (`weights`) animation; `JOINTS_1`/`WEIGHTS_1` (>4 influences). Each extends the same seams.

## Reuse vs build

**Reuse:** the `VertexSkin` stream + `.smesh v2` additive-section precedent; `jointMatrices` /
`updateWorldTransforms` / `worldMatrix`-`worldTranslation`-`worldRotation`; the `simTick` seam +
`tickPlay` gating/clamp + the play-scene-duplicate-discarded-on-stop model; `registerComponent<T>` +
`registerBuiltinComponents` + the `gen.ts` component-serde codegen (the `ScriptComponent` /
`MaterialSetComponent` nested-data precedents); the per-frame joint palette SSBO + `ensureJointCapacity`
+ `submitDrawList` + the `Instancing` grow-only buffer pattern; the `addPass`/`RgUsage` barrier-deriving
graph; the native overlay (`addLine`/`addCircleFill`/`submitOverlay` + `OverlayState` ring); the
`registerCommand<Params,Result>` + DTO codegen + version-stamped reconcile poll + `se` CLI + e2e harness;
the right-"tools" dock model + `layoutBus`/`emitLayoutSettled` bounds-sync; `FrameTimeGraph`'s canvas
pump + `useScrubValue` + `makeCoalescer` + `Tabs variant="line"` + the oklch dark tokens.

**Build new:** `Saffron.Animation` (pose/`PoseBuffer`, samplers, blend layer, evaluator, transitions);
the `Clip`/`Track` types + glTF animation walk + `.sanim` IO in `Saffron.Geometry`;
`AnimationPlayerComponent` + serde + registration + `AssetType::Animation`; the Host `simTick` composition
+ one-shot eval seam; the `skin.slang` compute kernel + deformed-vertex buffers + palette double-buffer +
`prevModel` + the per-pass rewiring + skinned BLAS rebuild; `buildSkeletonOverlay`; the animation control
commands + `se` formatter + `animationVersion`; the bottom-dock infra + the canvas `TimelinePanel`; the
two-bone IK solver + reserved per-bone physics metadata; the e2e tests + docs pages.

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`). Mark a phase
`COMPLETED` only when validation-clean (`make engine` + `make prepare-for-commit`; editor phases also
`bun run check`; wire/runtime phases also `make e2e`); delete a phase file only *after* it is `COMPLETED`
and merged. Per the root `AGENTS.md` "keep current" rule, each phase ends by adding/updating its
`docs/content/` page, its e2e coverage, and its `se`/control command in the same change. Phase 1 also
updates the module DAG in `AGENTS.md`.

## Phases

| Phase | What | Status |
|---|---|---|
| [1 — `Saffron.Animation` module + pose/clip types + samplers](phase-01-animation-module-and-types.md) | new module (DAG edge, classic-include); `Pose`/`PoseBuffer`/`Clip`/`Track` types; STEP/LINEAR/CUBICSPLINE sampling + slerp; builds in isolation, sampling is unit-testable. | COMPLETED |
| [2 — glTF clip import → `.sanim` + `AssetType::Animation`](phase-02-gltf-clip-import-and-sanim.md) | walk the ignored `data->animations[]`; `.sanim` (`SANM`) writer/loader in Geometry; one `AssetType::Animation` catalog entry per clip, bound by joint name+index; spawn `AnimationPlayerComponent` on import. | COMPLETED |
| [3 — `AnimationPlayerComponent` + evaluator + blend layer + playback](phase-03-player-component-and-runtime.md) | dumb-data component (+serde+registration); the `Saffron.Animation` evaluator (sample → `PoseBuffer` → **inert blend layer** → world-transform composition, rest pose untouched); evaluated in **Edit (preview) and Play**, animation before scripts; once/loop/ping-pong + speed. **Playback + non-destructive Edit preview work.** | COMPLETED |
| [4 — transitions: cross-fade + inertialization](phase-04-transitions-and-blending.md) | two-clip cross-fade by per-joint alpha; inertialization (single-clip + quintic pose-offset decay) as the default transition, reusing the pose-diff machinery the physics handoff needs. | COMPLETED |
| [5 — control plane + `se` CLI + e2e + `animationVersion`](phase-05-control-plane-and-cli.md) | DTOs + `AssetTypeDto::Animation`; `list-clips`/`play-animation`/`pause`/`seek-animation`/`set-loop`/`get-animation-state`; `animationVersion` stamp + one-shot eval-on-seek; `se` verbs; `tests/e2e/animation.test.ts` + a rigged+animated fixture. **Scriptable & gated.** | COMPLETED |
| [6 — compute skinning pre-pass + deformed-vertex buffer](phase-06-compute-skinning-prepass.md) | `skin.slang` compute kernel; grow-only per-frame deformed-vertex buffer (32-B `Vertex` layout); scene pass reads it on binding 0; `vertexMainSkinned` retired for the scene pass. | COMPLETED |
| [7 — wire all geometry passes to the deformed buffer](phase-07-passes-read-deformed-buffer.md) | drop the `batch.skinned` guards in depth prepass / shadow / SSAO G-buffer; flip the scene-pass depth store-op so skinned depth survives; characters self-shadow + get AO + early-Z. | COMPLETED |
| [8 — skinned motion vectors (palette double-buffer + `prevModel`)](phase-08-motion-vectors.md) | previous-frame palette + previous deformed buffer; add `prevModel` to `InstanceData`; `motion.slang` reads the prev deformed position; skinned + moving meshes stop ghosting under TAA. | COMPLETED |
| [9 — skinned BLAS rebuild for ray tracing](phase-09-skinned-blas-rebuild.md) | per-frame `UPDATE`-mode BLAS rebuild from the deformed buffer for skinned meshes; characters deform in RT shadows / DDGI / ReSTIR. | COMPLETED |
| [10 — skeleton line overlay + `show-bones`](phase-10-skeleton-overlay.md) | `buildSkeletonOverlay` (segments + joint dots + RGB axis lines, screen-constant); renders in Edit **and** Play; selection highlight; a `SceneEditContext` option + `set-skeleton-overlay` control command. | COMPLETED |
| [11 — editor bottom dock + tools-style tab system](phase-11-editor-bottom-dock.md) | `BottomTool` union + `bottomTools`/`activeBottomTool` slices + open/close + height persistence; `Layout.tsx` full-width bottom dock; `Topbar` trigger; `emitLayoutSettled` bounds-sync. Empty dock that opens/closes/resizes and re-glues the viewport. | COMPLETED |
| [12 — read-only canvas `TimelinePanel`](phase-12-timeline-panel.md) | track headers / ms ruler / clip bars / scrubbable playhead / transport + Loop / footer; canvas pump like `FrameTimeGraph`; reads `get-animation-state` on the reconcile poll; scrub → `seek-animation` via `useScrubValue` + coalescer. **The headline UI.** | NOT STARTED |
| [13 — kinematic foot IK + reserved physics metadata](phase-13-foot-ik-and-physics-ahead.md) | two-bone analytic IK as the first blend-layer override producer (foot planting against a ground hook); reserve per-bone physics fields (shape/mass, joint type, swing/twist limits, PD gains) on the skeleton for the eventual Jolt powered ragdoll. | NOT STARTED |

## Sequencing

Strictly dependency-ordered, lowest-risk first, each phase independently shippable.

**1–2** are the data foundation: the module + types build in isolation (1), then import fills the clips
(2). **3** is the milestone where *a clip visibly plays* — it wires the evaluator + the inert blend layer
into `simTick`; everything after is a producer into, or a consumer of, that pose path. **4** adds smooth
transitions. **5** makes it scriptable and adds the e2e gate (and the rigged fixture the gate needs) —
the natural cut line before any UI.

**6–9 (compute skinning) and 10–12 (editor) are independent of each other** once 1–5 land, so two agents
can run them in parallel. The 6→7→8→9 order within the rendering block is itself dependency-ordered (the
deformed buffer must exist before passes read it, before its previous-frame twin feeds motion, before the
BLAS rebuilds from it). The editor block is 11 (the empty dock) → 12 (the timeline inside it); **10 (the
skeleton overlay)** is engine-side and independent — it only needs bones, so it can land any time after 3.

**13** is last by design: it is the forward-looking proof that the Phase-3 blend layer admits an external
pose producer, and the place to reserve the Jolt-ahead metadata — valuable to validate the architecture,
but not on the critical path to "animations play and the timeline is polished."

A reasonable **first visible slice** is **1 + 2 + 3** (a rigged glTF imports and its clip plays on Loop —
in Play, or previewed in Edit via a runtime flag — even before correctness passes and UI; the timeline
transport that drives Edit preview properly arrives in 5/12). A reasonable **first demo-able slice** is
**1–5 + 10** (plays, loops, scriptable, with a skeleton overlay you can watch move). The headline editor
experience is **+11 + 12**; visual correctness for cinematic shots is **+6–9**.
