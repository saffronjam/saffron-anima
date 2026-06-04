# Phase 3: Adopt world transform across renderer, picking, camera, gizmo, billboards

**Status:** NOT STARTED
<!-- Flip to COMPLETED when every "## Done when" box is checked and a headless SAFFRON_EXIT_AFTER_FRAMES run is validation-clean. Delete this file only after COMPLETED + merged. -->

## Goal

Switch every transform *consumer* from the local `transformMatrix(transform)` to the cached world
matrix from phase 1 (`updateWorldTransforms`/`worldMatrix`/`worldTranslation`/`worldRotation`), so
parenting actually applies in rendering, picking, the lights, reflection probes, the play camera, the
native gizmo, and the editor billboards. The migration lands in **one change**: the scene AABB,
directional shadow fit, and DDGI volume fit accumulate inside the same mesh loop, so a half-migration
erupts or shrinks the shadow frustum. Reads (draw, pick) and writes (gizmo drag) must adopt identical
world↔local semantics or visuals diverge from picks.

Depends on phase 1 (the `RelationshipComponent` durable parent uuid, `relinkHierarchy`,
`WorldTransformComponent`, `updateWorldTransforms(Scene&)`, and the `worldMatrix`/`worldTranslation`/
`worldRotation` readers). This phase introduces no new components or schemas of its own — it is a
consumer migration plus the gizmo world→local rebase. The reparent command, `parentId` wire field, and
the React tree are owned by other phases; the representation and propagation decisions are settled in
phase 0 and are not re-litigated here.

## Current state

Every transform is local-treated-as-world today. The consumers, all reading `transformMatrix(transform)`
or `.translation`/`glm::quat(.rotation)` directly:

- **Mesh draw** — `renderScene` mesh loop, `forEach<TransformComponent, MeshComponent>` at
  `assets.cppm:792`; `const glm::mat4 model = transformMatrix(transform)` at `assets.cppm:822` feeds the
  per-draw world AABB (`assets.cppm:826-846`), the scene AABB `sceneMin`/`sceneMax`, `DrawItem.model` +
  `normalMatrix = transpose(inverse(mat3(model)))` (`assets.cppm:853-854`), and transitively the
  instance SSBO, TLAS `rtModels`, DDGI box proxy, and directional-shadow fit.
- **Point lights** — `forEach<TransformComponent, PointLightComponent>` at `assets.cppm:727`;
  `gpu.positionRange = vec4(transform.translation, range)` (`assets.cppm:732`); first light seeds
  `pointShadowPos = transform.translation` (`assets.cppm:738`).
- **Spot lights** — `forEach<TransformComponent, SpotLightComponent>` at `assets.cppm:748`;
  `gpu.positionRange = vec4(transform.translation, ...)` (`assets.cppm:754`); first spot builds
  `lightView = glm::lookAt(transform.translation, transform.translation + dir, up)` (`assets.cppm:766`),
  `dir = normalize(light.direction)` (`assets.cppm:752`) — the direction is a *component field*, not
  derived from the transform rotation.
- **Directional light** — no transform read (direction is a component field), `assets.cppm:705-717`.
- **Reflection probes** — `forEach<TransformComponent, ReflectionProbeComponent>` at `assets.cppm:916`;
  `up.origin = transform.translation` (`assets.cppm:927`) places each probe by its Transform — a renderer
  transform consumer that must adopt the world matrix alongside draw, pick, and lights.
- **Picking (mesh)** — `pickEntity` (`assets.cppm:1026`), `forEach<TransformComponent, MeshComponent>` at
  `assets.cppm:1051`; rebuilds the world AABB from `const glm::mat4 model = transformMatrix(transform)` at
  `assets.cppm:1061` — a second, independent local-transform consumer that must stay in lockstep with the
  draw loop.
- **Play camera** — `primaryCamera` at `scene.cppm:345`; builds `model = translate(translation) *
  mat4_cast(quat(rotation))` then `result.view = inverse(model)` (`scene.cppm:355-357`) — ignores scale,
  rebuilds inline (not via `transformMatrix`).
- **Native gizmo** — `gizmoAxes(transform, space)` at `scene_edit_gizmo.cpp:153` (Local basis =
  `glm::quat(transform.rotation)`, the *local* rotation, `:159-160`); `hitNativeGizmo` (`:171`) projects
  `transform.translation` at `scene_edit_gizmo.cpp:179`/`:185`/`:191`/`:201`/`:224-235`;
  `applyNativeGizmoDrag` (`:246`) writes `transform.translation`/`rotation`/`scale` directly in world
  space (translate `:291`, rotate `:298-300`, scale `:304-313`).
- **Host overlay billboards** — `buildSceneEditBillboards` at `host.cppm:191`; point glyph
  `transform.translation` (`host.cppm:209`), spot glyph (`host.cppm:227-228`) + forward
  `glm::quat(t.rotation) * vec3(0,0,-1)` (`host.cppm:237`), camera glyph `transform.translation`
  (`host.cppm:252`); the gizmo overlay itself reads the local transform at `host.cppm:112`.
- **Host SDL drag-begin snapshot** — inside `handleNativeGizmoPointer`, `host.cppm:307-311` snapshots
  `startTranslation`/`startRotation`/`startScale` from the entity's own transform; the mesh ray-pick call
  is `host.cppm:316` (the event sink that invokes `handleNativeGizmoPointer` is registered at
  `host.cppm:444-451`).
- **Control gizmo-pointer drag-begin snapshot** — `gizmo-pointer` "begin" phase at
  `control_commands_scene.cpp:853`; snapshots `startTranslation`/`startRotation`/`startScale` from the
  entity transform at `control_commands_scene.cpp:865-867`.
- **Control billboard-pick** — `pickBillboard` (`control_commands_scene.cpp:74`) projects
  `getComponent<TransformComponent>(...).translation` at `control_commands_scene.cpp:90`.
- **Control focus** — `focus` (`control_commands_scene.cpp:469`) reads `transform.translation`,
  `control_commands_scene.cpp:481`.

The canonical `renderScene` lives in `Saffron.Assets` (`assets.cppm`), *not* `renderer.cppm` — a plan
that edits "the renderer" misses it. `transformMatrix` (`scene.cppm:119`) stays the local builder,
unchanged.

## Implementation

### 1. Drive the per-frame flatten pass before any consumer reads

Call `updateWorldTransforms(scene)` exactly once per frame, *before* `renderScene` gathers draws, so
every consumer below reads a consistent cache (parent-before-child, see phase 1). `renderScene` is the
natural choke point: every per-frame consumer (mesh loop, lights, scene AABB) runs inside it, and
`pickEntity` runs on demand after it.

- Add the call at the top of `renderScene` (`assets.cppm`, before the light loops at `:727`), so the
  `WorldTransformComponent` cache is fresh for the whole gather. `updateWorldTransforms` is idempotent
  within a frame; the gizmo/pick paths that run between frames read the same cache (no parent moved
  since the last flatten unless a drag wrote a local transform — and the drag rebases against the
  *parent* world, which a child drag never changes, so the cache stays valid for the dragged entity's
  own subtree until the next flatten).
- `pickEntity` (`assets.cppm:1026`) runs from the SDL pick path and the control `pick` command between
  renders; it must read the cache written by the last `renderScene`. Since `renderScene` runs every
  frame and the pick fires on a pointer event after a render, the cache is current. Do **not** add a
  second flatten inside `pickEntity` — one pass per frame is the v1 contract; a stale-cache fallback
  (ancestor walk) already lives in `worldMatrix` per phase 1.

### 2. Mesh draw loop → world matrix (the load-bearing edit)

In the `renderScene` mesh loop (`assets.cppm:792`), replace `transformMatrix(transform)` at
`assets.cppm:822` with `worldMatrix(scene, entity)`:

```cpp
const glm::mat4 model = worldMatrix(scene, entity);
```

Everything downstream consumes the composed `model` and needs **no** further edit (editing them would
double-apply the parent): the per-draw + scene AABB corners (`:826-846`), `DrawItem.model` (`:853`),
`normalMatrix = transpose(inverse(mat3(model)))` (`:854`, stays correct because full mat4 composition
preserves non-uniform parent scale), the DDGI `boxMins`/`boxMaxs`/`boxAlbedos`, the instance SSBO via
`submitDrawList`, the TLAS `rtModels`, and the directional-shadow frustum fit to the scene AABB. This is
the single edit that makes the scene AABB / shadow / DDGI correct atomically — they all derive from
`model`.

### 3. Lights → world translation; decide direction

Point lights (`assets.cppm:727-744`):

```cpp
const glm::vec3 pos = worldTranslation(scene, entity);
gpu.positionRange = glm::vec4(pos, light.range);
// pointShadowPos = pos;  for the first light
```

Spot lights (`assets.cppm:748-774`): use `worldTranslation(scene, entity)` for `positionRange` (`:754`)
and both `lookAt` origins (`:766`). The spot/directional `direction` is a component field, not derived
from rotation. **Decision (this phase commits):** world-rotate the direction so a parented light re-aims
with its parent — compose the entity's world rotation onto the component direction:

```cpp
const glm::vec3 dir = glm::normalize(worldRotation(scene, entity) * light.direction);
```

Apply the same `worldRotation(scene, entity) * direction` to the directional light's component direction
at `assets.cppm:705-717` (it has a `TransformComponent` only if authored with one; guard with
`hasComponent<TransformComponent>` and fall back to the raw `light.direction` for an unparented/transformless
directional light). Skipping this leaves parented lights translating but never re-aiming — the deliberate
choice is to re-aim. Position is unconditional; direction composition is the design call recorded here.

### 3a. Reflection probes → world translation

`renderScene` snapshots each `ReflectionProbeComponent` into the renderer in the
`forEach<TransformComponent, ReflectionProbeComponent>` loop (`assets.cppm:916`), placing the probe by
its Transform: `up.origin = transform.translation` (`assets.cppm:927`). Replace that read with
`worldTranslation(scene, entity)` so a parented probe captures at its parent-composed world origin. The
probe carries a position and an influence zone, not a rotation, so only the translation rebases — this is
a translation consumer in lockstep with the lights, and a missed probe places its capture at the child's
local origin while the mesh it should reflect renders at its world position.

### 4. Picking AABB → world matrix (lockstep with draw)

In `pickEntity` (`assets.cppm:1051`), replace `const glm::mat4 model = transformMatrix(transform)` at
`assets.cppm:1061` with `worldMatrix(scene, entity)`. This is the second independent consumer of the
same matrix; it **must** change in the same commit as step 2 or picks land at the child's local origin
while the mesh renders at its world position. The slab test and corner loop are unchanged.

### 5. Play camera view → inverse(world)

In `primaryCamera` (`scene.cppm:345`), build the view from the camera entity's world transform instead
of the inline `translate * mat4_cast` at `scene.cppm:355-356`:

```cpp
result.view = glm::inverse(worldMatrix(scene, Entity{ /* the entity handle */ }));
```

The `forEach<TransformComponent, CameraComponent>` lambda receives the `Entity` (currently discarded —
the lambda signature is `[&](Entity, TransformComponent&, CameraComponent&)`); name it and pass it to
`worldMatrix`. The host viewport uses the editor fly-cam, so this only affects the play-camera path
today, but it is still a local-transform consumer that breaks a *parented* camera. Note this now honors
parent scale via the world matrix; the prior inline path ignored scale — for an unparented camera the
result is identical (TransformComponent scale defaults to 1).

### 6. Gizmo: operate at world position/rotation, write rebased local TRS

The gizmo is the highest-risk edit. It draws and hit-tests in **world** space but writes to the child's
**local** `TransformComponent`. The three sites (`scene_edit_gizmo.cpp`, the host SDL snapshot, the
control gizmo-pointer snapshot) must adopt identical semantics.

6a. **`gizmoAxes`** (`scene_edit_gizmo.cpp:153`): the Local-space basis must use the entity's **world**
rotation, not `glm::quat(transform.rotation)`. Change the signature to take the world rotation (or the
`Scene&` + `Entity` to fetch `worldRotation`); World space stays the identity basis. The cleanest seam:
pass the already-resolved `glm::quat worldRot` into `gizmoAxes` from the callers, so the function stays
pure.

6b. **`hitNativeGizmo`** (`scene_edit_gizmo.cpp:171`): project the entity's **world** translation
(`worldTranslation(editor.scene, editor.selected)`) at `:179`, `:185`, and every `transform.translation`
in the axis/plane/ring projections (`:191`, `:201`, `:224-235`); feed `gizmoAxes` the world rotation.

6c. **`applyNativeGizmoDrag`** (`scene_edit_gizmo.cpp:246`): the drag math runs in world space —
`gizmo.startTranslation` must be the entity's **world** translation at begin (see 6d), `gizmoAxes` uses
the world rotation, and `unitsPerPixel`/`projectedAxis` already key off `gizmo.startTranslation`
(`:257`, `:262-263`). Then **rebase the world result into the child's local frame** before writing
`TransformComponent`:
  - **Translate** (`:291`): compute the world target `worldPos = gizmo.startTranslation + move`, then
    `transform.translation = vec3(inverse(parentWorld) * vec4(worldPos, 1))`, where `parentWorld =
    worldMatrix(scene, parent)` (identity when the entity is a root, so unparented behavior is
    byte-identical to today).
  - **Rotate** (`:298-300`): apply the world delta to the world rotation, then peel off the parent:
    `localRot = inverse(quat(parentWorldRotation)) * worldRot` and store its Euler back to
    `transform.rotation`. For an unparented entity this reduces to the current `startRotation + delta`.
  - **Scale** (`:304-313`): scale is applied in the entity's own local frame already; under a non-sheared
    parent the local scale is unaffected by parent rotation/translation, so keep writing
    `transform.scale = gizmo.startScale * factor`. Decomposing world scale back through a sheared parent
    is lossy (TRS-only model, accepted per phase 0); document that scale stays local.

  Helper: the parent handle comes from `RelationshipComponent.parentHandle` (phase 1 runtime cache);
  resolve `parentWorld` once at drag begin and stash it, or recompute per tick from the cache (cheap).
  Add a `glm::mat4 startParentWorld` field to `NativeGizmoState` (`scene_edit_context.cppm`, alongside
  `startTranslation`/`startRotation`/`startScale`) so the rebase uses a *frozen* parent world for the
  whole drag — recomputing the parent world each tick is fine only if the parent does not move during
  the drag, which holds for a single-entity drag.

6d. **Snapshot world at drag begin, in both paths, identically.** Both snapshot sites must capture the
entity's **world** translation/rotation (and the frozen `startParentWorld`):
  - Host SDL: `host.cppm:307-311` (inside `handleNativeGizmoPointer`) — set
    `gizmo.startTranslation = worldTranslation(editor.scene, target)`,
    `gizmo.startRotation = eulerOf(worldRotation(...))`, `gizmo.startParentWorld = parentWorldOf(...)`.
  - Control gizmo-pointer "begin": `control_commands_scene.cpp:865-867` — the same three assignments
    against `ctx.sceneEdit`. These two must stay byte-for-byte equivalent in semantics or a CLI/Tauri
    drag diverges from an SDL drag. `gizmo.startScale` stays the local scale in both (per 6c).

### 7. Host overlay billboards → world translation/rotation

In `buildSceneEditBillboards` (`host.cppm:191`):
- Point glyph (`host.cppm:209`): `worldTranslation(editor.scene, e)`.
- Spot glyph + forward (`host.cppm:227-228`, `:237`): anchor at `worldTranslation`; forward =
  `worldRotation(editor.scene, e) * glm::vec3{0,0,-1}` (replacing `glm::quat(t.rotation) *`).
- Camera glyph (`host.cppm:252`): `worldTranslation(editor.scene, e)`.

Else billboards detach from parented entities while the mesh/gizmo move.

### 8. Control billboard-pick + focus → world translation

- `pickBillboard` (`control_commands_scene.cpp:74`): project `worldTranslation(editor.scene, e)` at
  `:90` so CLI/Tauri billboard picks match the viewport glyph positions.
- `focus` (`control_commands_scene.cpp:469`): aim the editor camera at
  `worldTranslation(ctx.sceneEdit.scene, *entity)` (the local read it replaces is at `:481`).

These mirror steps 6–7 so every pick/focus path agrees with what is rendered.

### 9. No wire/schema change in this phase

This phase adds no command, no DTO, no `parentId` field — those belong to the reparent/tree phases. The
DTO source of truth (`control_dto.cppm`), the generator (`tools/gen-control-dto/gen.ts`) and its
outputs (the C++ serde, `editor/src/protocol/se-types.ts`, `schemas/control/openrpc.generated.json` +
`command-manifest.generated.json`), the `tools/check-control-schema` contract test, and `tools/se` are
therefore **untouched here**; the DTO-first obligation is satisfied by the phases that add
`set-parent`/`parentId`. The verification for this phase is behavioral (renders/picks/round-trips), not
contract-shape. If any edit in steps 6–8 *changes the JSON shape* of `pick`/`gizmo-pointer`/`focus`
responses (it should not — these return the existing `EntityRef`/op-name DTOs), then the DTO, the
regenerated outputs, the contract test, and `se` formatters must be updated in the same change; the
intended outcome is no wire change.

## Done when

- [ ] A parented child mesh renders at its parent-composed world position: a headless render
      (`SAFFRON_EXIT_AFTER_FRAMES=N`) of a scene where child `B` is parented to `A` shows `B` at `A.world
      * B.local`, not at `B.local`.
- [ ] `pickEntity` hits a parented child at its rendered world location, not its local origin: an
      `tests/e2e` driver loads a parented scene, issues `pick` at the child's *world* screen position,
      and gets the child's id back (and a `pick` at the child's *local* origin misses).
- [ ] Dragging the gizmo on a parented child moves it correctly in world space and writes a sensible
      local TRS: drive `gizmo-pointer {begin,drag,end}` over the control plane on a parented child, then
      `inspect` — the child's local `TransformComponent` differs from its pre-drag local value, and the
      child's `worldTranslation` matches the drag target.
- [ ] Re-load reproduces the dragged placement: `writeScene` → `readScene` (or save/load via control)
      round-trips the post-drag scene and the child's world position is unchanged (the rebased local TRS
      is durable).
- [ ] The play camera, when its `CameraComponent` entity is parented, produces a view whose inverse
      translation equals the camera entity's `worldTranslation` (assert in `tests/e2e` or the scene
      self-test).
- [ ] Unparented entities are byte-identical to pre-phase behavior: an existing flat scene renders and
      picks the same (root `parentWorld == identity`).
- [ ] Billboards, billboard-pick, and `focus` all target the same world position as the rendered glyph
      for a parented light/camera (driven via `se`/control against a parented scene).
- [ ] A headless `SAFFRON_EXIT_AFTER_FRAMES` run over a parented scene is **validation-clean** (no
      Vulkan validation errors; the directional-shadow / DDGI fit does not erupt or collapse).

## Risks / seams

- **Half-migration erupts the shadow frustum.** The scene AABB, directional-shadow fit, and DDGI volume
  fit all accumulate from the mesh loop's `model` (`assets.cppm:842-848`). Every renderable must switch
  in the same commit (step 2 covers them via the single `model` source). Missing one renderable, or
  switching draw but not pick, makes the shadow frustum or picks diverge.
- **Two drag-begin snapshots must stay in lockstep.** `host.cppm:307-311` (SDL, inside
  `handleNativeGizmoPointer`) and `control_commands_scene.cpp:865-867` (gizmo-pointer "begin") capture
  the same start state; both must adopt the identical world-snapshot + frozen `startParentWorld`
  semantics or an SDL drag and a CLI/Tauri drag produce different local TRS for the same gesture.
- **World→local inversion is numerically delicate.** `inverse(parentWorld)` is ill-conditioned under
  zero or near-zero parent scale and lossy under sheared/non-uniform parents when decomposing a world
  rotate/scale back to child Euler + vec3 scale. The TRS-only model (phase 0) accepts this: scale stays
  local (6c), and rotation peels the parent's rotation only. Document that shear is not inherited.
- **Direction is a deliberate choice.** World-rotating the light/directional direction (step 3) is a
  design commitment; if a future maintainer reverts it, parented lights translate but never re-aim — the
  failure is silent (light just points the original way). Keep the `hasComponent<TransformComponent>`
  guard so a transformless directional light still works.
- **Cache freshness across frames.** `updateWorldTransforms` runs once per frame inside `renderScene`;
  `pickEntity`/gizmo run on pointer events after the render, reading that cache. A gizmo drag writes a
  *child* local transform between frames, which does not invalidate the *parent* world the rebase reads,
  so the cache stays valid for the dragged subtree until the next flatten. The `worldMatrix` ancestor-walk
  fallback (phase 1) covers any genuinely-stale read.
- **`primaryCamera` now honors parent scale.** The inline path ignored scale; `worldMatrix` includes it.
  For unparented cameras (scale 1) the view is unchanged, but a parented-and-scaled camera now inherits
  scale into its view basis — intended, but call it out so it is not read as a regression.
- **`pickEntity` must not double-flatten.** Adding a second `updateWorldTransforms` inside `pickEntity`
  would re-walk the tree per pick; rely on the per-frame pass and the stale-cache fallback instead.
