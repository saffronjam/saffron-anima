# Phase 4 — Node-TRS runtime (evaluator + spawn players + binding)

**Status:** NOT STARTED

**Depends on:** Phase 1 (AnimTrack/AnimClip generalization + `.sanim` v2 + mode-keyed sampler),
Phase 3 (morph delta storage + `.smesh` flags collapse + `MorphComponent`/`MorphWeightOverrideComponent`
+ spawn seeding).

## Goal

Make `tickAnimation` drive bones, node transforms, and mesh morph weights through **one evaluator and
one write seam**. A sampled `JointPose` goes into a `PoseOverrideComponent` on whatever entity the track
targets — a bone *or* a node — and a sampled weights vector goes into a `MorphWeightOverrideComponent`
on the mesh entity. Node tracks bind by durable name, resolved once to a stable entity `Uuid` and cached
(keyed by the player uuid, mirroring `lastPose`), re-resolved by name on a cache miss so a binding
survives reparent and re-resolves on rename. Spawn attaches exactly one `AnimationPlayerComponent` per
model instance, including skinless animated models, so `BoxAnimated` plays its node animation through the
hierarchy.

`localMatrix` (scene.cppm:852-860) already prefers `PoseOverrideComponent` over `TransformComponent` for
**any** entity, and `updateWorldTransforms` (scene.cppm:914-944) already composes the parent chain — so a
driven node is just a node with a `PoseOverride`, and this phase writes **zero new compose code**. The
work is entirely in the evaluator's routing + binding, the spawn player attachment, and the clear path.

CPU-only. Node-TRS just writes overrides the existing static-mesh draw already follows; the name→Uuid
cache amortizes the subtree walk so a steady-state frame does map lookups, not a DFS.

## Why this is one code path (NO LEGACY)

There is exactly one evaluator (`tickAnimation`, animation.cpp:603-764), one per-channel sampler
(`sampleTrack`, animation.cpp:352), and one resolved-clip step (`sampleClipResolved`,
animation.cpp:308-349). This phase generalizes them — it does **not** add a `NodeTrsAnimationComponent`, a
`tickNodeAnimation`, or a parallel player. Node-TRS reuses the existing `AnimationPlayerComponent`
(scene.cppm:92-117) and the existing play/seek/loop/state commands unchanged. The only branch is *where
the sampled value is written*, decided per track from its target kind (Phase 1's `AnimTrack` target
discriminator + `Path::Weights`), not per component type.

## Pre-reqs this phase assumes from Phases 1 and 3

Must already exist before this phase begins — do not re-create or stub them here:

- **Phase 1:** `AnimTrack` carries a target discriminator (Bone vs Node) plus the durable target name
  (today's `jointName`, generalized), and a `Path::Weights`. `sampleTrack` handles the N-wide weights
  stream. `sampleClip`/`sampleClipResolved` route per channel; the weights sampler writes the N-wide
  vector into a caller-supplied span. This phase owns the weights destination (a scratch vector on
  `AnimationRuntime`, step 2) — Phase 1 adds **no** `PoseBuffer` weights field, so nothing is dead there.
- **Phase 3:** `MorphComponent` (durable, serialized, registered in `scene_edit_components.cpp`) and
  `MorphWeightOverrideComponent` (runtime-only, mirrors `PoseOverrideComponent`, removed on stop) exist;
  spawn seeds `MorphComponent` weights. `ImportedModel`/`ModelSpawnInput` expose whether a model has
  node-only animation and/or morph targets so spawn can branch.

If a pre-req is missing when this phase is implemented, finish that phase first.

## Ordered steps

### 1. Extend `AnimationRuntime` with the per-player node-binding cache

File: `engine/source/saffron/animation/animation.cppm` (`AnimationRuntime`, lines 104-115).

Add a binding cache keyed by the player entity uuid, the same shape as `lastPose`:

- `std::unordered_map<u64, NodeBinding>` where `NodeBinding` holds, per player: the resolved
  `targetName → entity Uuid` map for the clip's node tracks, the resolved morph mesh entity `Uuid`, the
  clip `Uuid` the binding was built against (to invalidate on a clip switch), and a one-time-warn flag
  set so duplicate-name / unresolved warns fire once per player rather than every frame.
- `///` doc the cache the house way: it amortizes the subtree name walk, survives reparent (the Uuid is
  durable), and re-resolves by name on a miss. It is cleared on project (re)load like `clipCache` /
  `transitions` / `lastPose`.

Add a free helper in the anonymous namespace of `animation.cpp`: DFS a subtree from a root entity
(through `RelationshipComponent.children`, scene.cppm:52-57) collecting `NameComponent.name → entity
Uuid`. **Duplicate names resolve to the first depth-first match with a one-time warn**; this matches the
bone `nameToIndex` first-write-wins policy (animation.cpp:645). This is the canonical UE-FGuid +
Unity-name-path hybrid: Uuid for durability, name for re-resolution.

### 2. Restructure `tickAnimation` to visit every player and branch on rig presence

File: `engine/source/saffron/animation/animation.cpp` (`tickAnimation`, lines 603-764).

Today `tickAnimation` iterates `forEach<AnimationPlayerComponent, SkinnedMeshComponent>` (line 605) — it
only visits rigged entities, so a skinless `BoxAnimated` player would never tick. Restructure so it
visits **every** `AnimationPlayerComponent`, then branches on the skin:

- Change the visit to `forEach<AnimationPlayerComponent>` and `try_get<SkinnedMeshComponent>` inside the
  body. Keep the shared preamble (active/`previewInEdit` gate at line 610, `loadClip` at 614, the IdComponent
  key at 616-620, the inactive-clip clear+erase at 621-627) — none of it depends on the skin.
- **Rigged** (`SkinnedMeshComponent` present): the existing bone path runs exactly as today — rest seeding
  (lines 632-648), `sampleClipResolved` for bone tracks (653), the transition/cross-fade/inertialize block
  (690-732), foot-IK (737-741), the per-bone `emplace_or_replace<PoseOverrideComponent>` write (743-758),
  and the `lastPose` snapshot (762) — **plus** the clip's node tracks and weights tracks routed through the
  shared write seam below. A skinned model that *also* has node tracks keeps its single mesh-entity player;
  its node bindings resolve against the whole model subtree.
- **Not rigged** (no `SkinnedMeshComponent`): no rest/bone machinery, no transition state keyed by the
  skeleton (a node-only clip has no bone pose to inertialize — keep transitions a no-op here in v1), no
  foot-IK, no `lastPose`. Only node tracks and weights tracks. This is the `BoxAnimated` path.

**One write seam.** Factor the bone-write at lines 754-757 into a single lambda taking an `entt::entity`
handle and a `JointPose`:

```
auto& over = scene.registry.emplace_or_replace<PoseOverrideComponent>(handle);
over.translation = pose.translation; over.rotation = pose.rotation; over.scale = pose.scale;
```

Both the bone loop (handle = `skin.boneHandles[i]`) and the node loop (handle = the binding-resolved node
entity) call it. A node track samples a `JointPose` via `sampleTrack` and writes it onto the bound node
entity's `PoseOverrideComponent`; the node then composes through `updateWorldTransforms` with no new code.

**Partial-channel nodes.** A node with only some channels animated (e.g. only translation) seeds the
override from the node's authored `TransformComponent` (its local TRS, Euler→quat via the same convention
as `restPoseOf`, animation.cpp:54-58) before overwriting the animated channels — so untracked channels
keep their authored value rather than snapping to identity. This mirrors how bones seed from
`restPoseOf` (line 638) then overwrite tracked channels.

**Weights tracks.** A `Path::Weights` track samples the N-wide vector (Phase 1 sampler) into a
runtime-owned scratch vector on `AnimationRuntime` (reused across tracks — see Performance), then writes a
`MorphWeightOverrideComponent` on the morph mesh entity — the `morphableDescendant` of the subtree (the
exact rule Phase 7's `morphableDescendant` helper formalizes; it is distinct from the bone/node
`animatableDescendant` because `MorphComponent` may sit on a different node than the rig). Size it to the
mesh's morph-target count from `MorphComponent`; if the clip's weight count disagrees, clamp and warn
once. For a node-only model with no morph, this never fires.

### 3. Resolve node + morph bindings once, against the model subtree root

In the `tickAnimation` body, before the per-track write loop, populate the player's `NodeBinding`:

- **Subtree root:** walk up `RelationshipComponent.parentHandle` (scene.cppm:55) from the player entity to
  the topmost ancestor — the model container `spawnSkinnedModel`/`spawnModel` create
  (assets.cppm:4905-4918). DFS its children to bind. Walking from the container lets a node track target
  any node in the model, whether the player sits on the mesh entity (skinned) or the container (skinless).
- For each node track, look up `targetName` in the cached map; on miss, re-walk the subtree (names may have
  changed). Resolve the morph mesh entity once with the same `morphableDescendant` rule Phase 7 formalizes —
  the subtree entity carrying `MorphComponent` — so the runtime and the `set-morph-weights`/`get-morph-weights`
  commands resolve the identical entity.
- At write time convert `Uuid → entt::entity` via `findEntityByUuid` and guard `valid(scene, …)`. A `Uuid`
  that no longer resolves (entity destroyed) triggers a name re-resolve; still unresolved → warn once
  (using the binding's one-time flag) and skip that channel.
- Invalidate the whole binding when `player.clip` differs from the binding's stored clip uuid (a clip
  switch rebinds), the same way `transitions`/`lastPose` are erased when the clip goes inactive (lines
  621-627).

### 4. Extend `clearOverrides` and the stop path to revert node + morph overrides

File: `engine/source/saffron/animation/animation.cpp` (`clearOverrides`, lines 62-71; the inactive-clip
branch, lines 621-627).

Today `clearOverrides(scene, skin)` removes `PoseOverrideComponent` from `skin.boneHandles` only. The
revert is the removal — **no snapshot/restore** (the override component IS the non-destructive layer):

- Change `clearOverrides` to take the player entity (and/or its resolved `NodeBinding`) so it can iterate
  the bound **node** entities and remove their `PoseOverrideComponent`, and remove the
  `MorphWeightOverrideComponent` from the bound morph mesh entity. For a rigged player it still clears bone
  overrides via `skin.boneHandles`.
- The inactive branch (clip == nullptr, lines 621-627) calls the extended clear and erases the player's
  `transitions`, `lastPose`, **and** `NodeBinding` entries.
- Confirm the stop-preview command path lands on this one clear. The stop/seek-to-stop flow flips
  `previewInEdit`/`playing` and the next `tickAnimation` inactive branch clears — verify in
  `control_commands_animation.cpp` (the `playerOf`/`stateOf` paths) that nothing clears bone overrides
  directly bypassing this; if it does, route it through the extended clear so node + morph revert too.

### 5. Attach exactly one player per instance at spawn, including skinless animated models

File: `engine/source/saffron/assets/assets.cppm` (`spawnSkinnedModel`, lines 4818-4970; `spawnModel`,
lines 4972-4982; `ModelSpawnInput`, lines 139-151).

Today `spawnModel` short-circuits a non-skin import to a single `MeshComponent` entity with **no node
forest and no player** (lines 4978-4981), and `spawnSkinnedModel` attaches the player to the mesh entity
when the rig ships clips (lines 4893-4899). Changes:

- **Skinless-but-animated:** Phase 0 lifts the flatten-into-world bake so an unskinned multi-node glTF gets
  a live per-node entity forest; this phase assumes that forest exists. When `result.animations` is
  non-empty and `!result.hasSkin`, `spawnModel` instantiates the node forest exactly as
  `spawnSkinnedModel` does (lines 4820-4843: one entity per `ImportedNode`, TRS from the node, parent links
  via `RelationshipComponent.parent` uuid), wraps it under a container (lines 4905-4918), attaches
  `MeshComponent` to the mesh-bearing node, and attaches **one** `AnimationPlayerComponent` to the
  **container** (`player.clip = result.animations.front()`, `playing = false`, `wrap = Loop` — the same
  defaults as lines 4895-4898). A single-node static model still collapses to one entity with no player
  (unchanged).
- **Skinned with node tracks:** keep the single mesh-entity player (lines 4893-4899). Its binding now also
  resolves node tracks across the subtree. Do **not** add a second player.
- **Exactly one `AnimationPlayerComponent` per model instance** — assert/document it; the evaluator and the
  control commands assume one player per instance, and two players on one subtree fight.
- `ModelSpawnInput.animations` (line 150) is the single source for `player.clip` for both paths.

### 6. Host integration is unchanged

File: `engine/source/saffron/host/host.cppm` (`tickAnimation` call, lines 1488-1493; `clipLoader`
install, line 1005).

The host already calls `tickAnimation(state->animation, activeScene, dt, animMode)` in both Edit and Play
(1488-1493) and installs `clipLoader` (1005). No host change is needed — the evaluator now drives nodes +
morph because of the restructure, and the host's single tick covers all players (rigged and skinless).
`clearOverrides` is `namespace`-private to `animation.cpp`, so its signature change is invisible to the
host; verify no other TU references it.

## Frontend work

**None.** The player surfaces through the existing `get-animation-state` command; live morph values and
Timeline channel drill-down land in Phases 7/8. Do not add UI here. The existing Timeline/Clips/Inspector
keep working unchanged for the rigged path; a node-TRS model gains a player but no new panel.

## Control commands

Node-TRS **reuses** the existing `play`/`seek`/`set-loop`/`get-animation-state` commands unchanged — same
`AnimationPlayerComponent`, so **no new command and no `gen.ts` change in this phase**. Morph control
commands (`set-morph-weight`/`get-morph-weight`/`list-clip-bindings`) land in **Phase 7**. Adding a verb
that duplicates `play` would violate NO LEGACY.

Do not narrow the existing rig-gate predicate that decides which entities accept play; widening it to "is
animatable" (`AnimationPlayer || SkinnedMesh || Morph`) is Phase 8's job. This phase only ensures a
skinless animated container *has* an `AnimationPlayerComponent`, so the existing `AnimationPlayer`-keyed
command resolution (`control_commands_animation.cpp`, `playerOf`) already finds it.

## Performance

- CPU-only. Node-TRS writes `PoseOverrideComponent`s the existing static-mesh draw already follows; no new
  GPU work.
- The `NodeBinding` cache amortizes the subtree DFS: it runs once per player per clip change, then a
  steady-state frame does only map lookups (`targetName → Uuid → entt::entity`). The DFS is the cost the
  cache exists to avoid.
- The per-track write loop is bounded by the clip's track count; reuse a scratch N-wide weights vector on
  the runtime so the weights path does not re-allocate per frame.
- Visiting all players (not just rigged) adds a cheap `try_get<SkinnedMeshComponent>` per player; the entt
  view is O(players), negligible.

## Docs

Defer the dedicated `node-trs-animation.md` page to **Phase 9** (per the canonical plan). Do not add or
edit a docs page here. If `docs/content/explanations/animation/playback-runtime.md` states the runtime
"drives only skinned rigs", that wording is now wrong — but the rewrite is batched into Phase 9 alongside
the new concept page and the hub `_index.md` row, so leave it for Phase 9 (note it in this plan rather
than touching docs now).

## Tests

### Animation self-test (`runAnimationSelfTest`, animation.cpp:766+)

Add a **node-TRS case** with no GPU and no real glTF, in the existing self-test style:

- Build a 2-node parent/child scene (two entities, `TransformComponent` + `RelationshipComponent`, child's
  `parent` = parent uuid; call `relinkHierarchy`). Build an `AnimClip` with one node-target translation
  track bound to the child's name (LINEAR, two keys). Attach an `AnimationPlayerComponent` (on the parent
  or a container), prime an `AnimationRuntime` with a `clipLoader` returning the clip.
- Tick at `t`, assert the **child entity's `PoseOverrideComponent`** matches the sampled value at `t`
  (within `eps`, mirroring the existing `sampleTrack` assertions, animation.cpp:791-799).
- Call `updateWorldTransforms`; assert the child's **world transform composes through the parent**
  (`worldTranslation(child)` == parent world translation + sampled local).
- **Stop → revert:** flip the player inactive, tick; assert the child has no `PoseOverrideComponent` and
  `worldTranslation` reverts to the authored value.
- **Binding cases:** rename the bound node + tick → re-resolves by name; reparent the node under another
  parent + tick → still driven (binding by Uuid survives); add a second node with the same name → first
  depth-first resolution, one-time warn, no crash.
- **Regression:** a skinned rig (existing self-test scaffolding) with **no node tracks** behaves exactly as
  before — bone overrides written, no node/morph overrides emitted.

### Host check (`tests/e2e`, bun over the control plane)

The wire is JSON, so this belongs in `tests/e2e`:

- Spawn `BoxAnimated` via the existing import/spawn commands; `play-animation` on the container's player.
- Assert both boxes' world transforms animate over time (poll `get-animation-state` for playhead advance;
  read the two box entities' world transforms via the entity/transform query commands) and that the
  **child box composes through the parent** (child world = parent world × child local).
- `stop` / seek-to-rest → assert the overrides are removed and both transforms revert to the authored
  values.
- Regression in the same suite: a skinned rig fixture with no node tracks still plays bones identically.
- The `tests/e2e` harness already gates on a validation-clean log; keep it clean across the run.

If a `BoxAnimated` fixture is not yet wired into `tests/e2e`, add a minimal one here (Phase 9 expands the
fixture set on top of it).

## Acceptance criteria

- `tickAnimation` drives bones, nodes, and morph weights through **one evaluator and one write seam** (a
  sampled `JointPose` → `emplace_or_replace<PoseOverrideComponent>` for bones and nodes alike; a sampled
  weights vector → `MorphWeightOverrideComponent` on the mesh entity).
- Node tracks resolve **by name to a cached stable Uuid**, survive reparent (Uuid durable), and re-resolve
  on rename; duplicate names take the first depth-first match with a one-time warn; unresolved channels
  warn once.
- `clearOverrides` + the stop-preview path clear node + morph overrides so authored values revert
  (non-destructive, no snapshot/restore).
- `BoxAnimated` plays its node animation through the hierarchy (child composes through parent); a skinned
  rig with no node tracks is **unaffected** (same bone behavior).
- Exactly one `AnimationPlayerComponent` per model instance, including skinless animated models.
- `make engine` builds clean; `make prepare-for-commit` (format + lint) is clean; the animation self-test
  and the e2e case pass.
