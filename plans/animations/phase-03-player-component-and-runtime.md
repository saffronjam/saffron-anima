# Phase 3 — `AnimationPlayerComponent` + evaluator + blend layer + playback

**Status:** COMPLETED

## Goal

The milestone where **a clip visibly plays** — and previews non-destructively in Edit mode, the way UE5
(Persona / Sequencer) and Unity (Animation window / Timeline) preview animation without entering play. Add
the dumb-data `AnimationPlayerComponent` to `Saffron.Scene`, build the per-entity **evaluator** in
`Saffron.Animation` (sample the clip → fill a `PoseBuffer` → pass through the **inert per-bone blend
layer** → feed it into world-transform composition), and have the Host run it **every frame in both Edit
and Play**. The bone `TransformComponent`s keep the **rest pose** and are never overwritten, so preview
dirties nothing. Support looping / once / ping-pong and a speed multiplier.

## What exists to build on

- The pose is consumed by `updateWorldTransforms` (`scene.cppm:613`) → `jointMatrices` (`scene.cppm:650`,
  `worldMatrix(bones[i]) · inverseBind[i]`), run in `renderScene`. `worldMatrix` reads the cached
  `WorldTransformComponent`, composed from each entity's `TransformComponent` + the `RelationshipComponent`
  parent chain. **The only change rendering needs is for a bone's composition to use the animated local
  TRS instead of its rest-pose `TransformComponent` when the bone is being animated.**
- `WorldTransformComponent` is a **runtime-only cache, excluded from serialization** (verification:
  `serializeEntity` skips unregistered storage). The animated pose override follows the same rule —
  runtime-only, never serialized.
- `simTick` (`scene_edit_context.cppm:211`) is invoked by `tickPlay` (`scene_edit_play.cpp:179-203`) only
  during Play/Paused; the Host currently points it at the script runtime. `tickPlay` is called per-frame
  from host `onUpdate` **before render**. `activeScene(ctx)` is the play duplicate during Play, the edit
  scene during Edit.
- Component machinery: `registerComponent<C>(reg, name, drawFn, toJson, fromJson, removable)`
  (`scene.cppm:880`); built-ins registered in `registerBuiltinComponents` (`scene_edit_components.cpp:17-68`);
  serde generated into `scene_component_serde.generated.cpp` by `gen.ts`. The shipped **`ScriptComponent`**
  and **`MaterialSetComponent`** (`scene.cppm:109`) are the data-only-component precedents — copy one.
- `forEach<C>` / `getComponent<C>` / `hasComponent<C>` (`scene.cppm:355-386`); `registry.try_get<C>` for an
  optional component. `SkinnedMeshComponent.boneHandles` (`scene.cppm:78`) is the resolved entt-handle
  cache, rebuilt once per load/play by `relinkHierarchy` — **never** rebuild it per frame.
- The `AssetCatalog` + `AssetType::Animation` entries (Phase 2) live in `Saffron.Scene`, so the evaluator
  resolves a clip Uuid → `.sanim` path without depending on `Saffron.Assets`.

## Work

### 1. `AnimationPlayerComponent` (dumb data, in `Saffron.Scene`)

```cpp
struct AnimationPlayerComponent {
    Uuid clip;                       // the AssetType::Animation catalog entry to play
    f32 time = 0.0f;                 // playhead, seconds
    f32 speed = 1.0f;
    enum class Wrap : u8 { Once, Loop, PingPong } wrap = Wrap::Loop;
    bool playing = false;            // advance time? (set by the game loop in Play, the timeline in Edit)
    bool previewInEdit = false;      // RUNTIME-ONLY (serialize as false): is this entity previewed in Edit?
    bool pingForward = true;         // ping-pong direction state (runtime)
    // transition state (Phase 4 fills these; harmless here):
    Uuid prevClip; f32 transition = 0.0f; f32 transitionDuration = 0.0f;
};
```

Register it in `registerBuiltinComponents` (after line 68) via `registerComponent<AnimationPlayerComponent>(...)`;
add `animationPlayerComponentToJson`/`...FromJson` following the `ScriptComponent` precedent and regenerate
serde with `gen.ts`. It is **data-only** — no Animation/Geometry knowledge — so `Saffron.Scene` gains no
new edge. (`previewInEdit`/`pingForward`/transition state are runtime; serialize defaults.)

### 2. The `PoseBuffer` override mechanism (approach P — non-destructive)

The evaluator does **not** write bone `TransformComponent`s. Instead it produces, per animated rig, a
final blended local TRS per joint and exposes it to world-transform composition via a **runtime-only**
override on the bone entities:

```cpp
/// Runtime-only (never serialized). Present on a bone entity while it is being animated.
struct PoseOverrideComponent { JointPose local; };   // JointPose from Phase 1
```

- The evaluator `emplace_or_replace<PoseOverrideComponent>` on each driven bone with the blended local TRS,
  and `remove`s it from a rig's bones when that rig stops being animated (so it reverts to rest pose).
- Extend `updateWorldTransforms` (`scene.cppm:613`): when composing a bone's local matrix, prefer
  `try_get<PoseOverrideComponent>(e)->local` over its `TransformComponent`. One `try_get` per transformable
  entity; negligible for non-animated scenes. This is the single rendering-side change, and it keeps the
  authored rest pose pristine. (Compose from the override's **quaternion** directly — no Euler round-trip,
  unlike writing `TransformComponent.rotation`.)

### 3. The evaluator (in `Saffron.Animation`)

```cpp
enum class AnimMode : u8 { Edit, Play };
/// Sample + (optionally) advance every active AnimationPlayerComponent, writing PoseOverrideComponents.
/// `catalog` resolves clip Uuids to .sanim paths; clips are loaded once and cached.
void tickAnimation(Scene& scene, const AssetCatalog& catalog, f32 dt, AnimMode mode);
```

Per entity with `AnimationPlayerComponent` **and** `SkinnedMeshComponent`:
1. **Active?** In `Play`, active = has the component (so the simulation animates every rig). In `Edit`,
   active = `previewInEdit` (so only the timeline-previewed entity animates; everything else stays at rest
   pose). Inactive rigs: ensure their `PoseOverrideComponent`s are removed (revert to rest), then skip.
2. **Advance time** only if `playing`: `time += dt * speed`; apply `Wrap` (`Once` clamp + stop at end;
   `Loop` `fmod`; `PingPong` bounce + flip `pingForward`). When not `playing` (a scrubbed/paused preview),
   leave `time` as set by `seek` — but still sample so the pose reflects it.
3. Load (cache) the `AnimClip` for `clip` via `loadAnimation`; size a `PoseBuffer` to the joint count once.
4. `sampleClip(clip, time, pose)` (Phase 1) → `pose.local`; joints with no track default to the bone's
   **rest** local TRS (read once from its `TransformComponent`).
5. **Resolve tracks to joints by name → index** against `SkinnedMeshComponent.bones` (each bone's
   `NameComponent`/`BoneComponent`), repairing a stale index; cache the resolution.
6. **Blend layer:** `final[i] = pose.weight[i] == 0 ? pose.local[i] : blendJoint(pose.local[i],
   pose.override_[i], pose.weight[i])` (lerp T/S, slerp R). All weights are 0 in v1 → `final == local`,
   but the call site exists so Phase 13 / physics only writes `override_` + `weight`.
7. `emplace_or_replace<PoseOverrideComponent>(boneEntity, JointPose{final[i]})` for each joint (resolve
   the bone via `SkinnedMeshComponent.boneHandles[i]`, guard `entt::null`).

### 4. Host wiring — run animation every frame, before scripts

In the host `onUpdate`, evaluate animation **before** `tickPlay` so that during Play the pose lands before
scripts run (and a script can still override a bone via the same `PoseOverrideComponent`):

```cpp
const auto mode = (editor.playState == se::PlayState::Edit) ? se::AnimMode::Edit : se::AnimMode::Play;
se::tickAnimation(se::activeScene(editor), assetCatalog, dt, mode);   // animation: Edit preview + Play
se::tickPlay(editor, dt);                                            // scripts via simTick (Play only)
```

Leave the script runtime on `simTick` as-is (Play only). Animation does not need the `simTick` seam — it
runs unconditionally each frame, gated internally by `mode`/`previewInEdit`/`playing`. During Play it still
lands before scripts because `tickAnimation` precedes `tickPlay`.

> `renderScene` re-runs `updateWorldTransforms`/`jointMatrices` downstream of `onUpdate`, so the override
> poses are picked up the same frame. Confirm `renderScene` runs after `onUpdate` in the host loop.

The timeline (Phase 12) / a control command (Phase 5) sets `previewInEdit = true` + `playing`/`time` on the
selected entity to drive Edit preview; clearing `previewInEdit` reverts it to rest pose next frame.

## Validation (done criteria)

- `make engine` + `make prepare-for-commit` clean.
- Headless: load the Phase-2 rigged fixture; (a) in **Edit**, set `previewInEdit=true` + `playing=true`,
  run `SAFFRON_EXIT_AFTER_FRAMES=N`, assert a tracked bone's **composed world transform** changed between
  frame 1 and N while its `TransformComponent` (rest pose) is **unchanged** and the project is **not
  dirtied**; clear `previewInEdit` and assert it reverts to rest. (b) in **Play**, assert it animates and
  Stop reverts cleanly.
- Manual: `make run`, import a rigged glTF; with the entity previewed it animates in Edit without entering
  Play and without a dirty marker; Play also animates it.
- `docs/`: add `docs/content/explanations/animation/playback-runtime.md` (sample → PoseBuffer → blend →
  world-transform composition; non-destructive Edit preview vs Play; looping modes; the UE5/Unity
  preview-decoupled-from-play parallel) + hub row.

## Notes / gotchas

- **Rest pose is sacred.** Bone `TransformComponent`s are never written by animation — they are the
  authored rest/bind pose. The animated pose lives only in `PoseOverrideComponent` (runtime-only). This is
  what makes Edit preview non-destructive *by construction* (no snapshot/restore) and avoids the Euler
  round-trip drift of writing `TransformComponent.rotation`.
- **Don't relink per frame.** `boneHandles` is resolved once by `relinkHierarchy` at load/play entry;
  reordering `bones[]` breaks indexing — the skeleton is immutable per session.
- **Play still uses the duplicate-and-discard scene model** for everything else (scripts, runtime spawns);
  animation just happens not to need it because it never mutates authored data.
- Clip cache keyed on clip Uuid (`unordered_map<u64, AnimClip>` owned by the evaluator), invalidated on
  asset reload.
- In Edit, default `previewInEdit=false` so importing a rig does **not** make it auto-animate in the
  viewport — it animates only when the timeline/selection previews it (matching UE/Unity, which don't
  auto-play level animation in-editor by default).
