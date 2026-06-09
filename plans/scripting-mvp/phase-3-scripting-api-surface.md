# Phase 3 — the scripting API surface

**Status:** COMPLETED

## Goal

Fill out what a script can read and do to the scene. Building on `self.entity` and `set_position` from
Phase 2, add: **find another entity by name**, **read any component as a table**, the remaining
**transform setters** (move/rotate/scale), and **move the camera**. All bindings stay inside
`Saffron.Script`, over the `Saffron.Scene` free-function facade — nothing else changes.

## What exists to build on

- The opaque `Entity { entt::entity handle }` (`scene.cppm:342`); `valid(scene, e)` (`scene.cppm:347`);
  the free-function facade `getComponent/hasComponent/forEach` (`scene.cppm:355,424`). `getComponent<C>`
  is UB if absent — **always `hasComponent`-guard**.
- `TransformComponent { glm::vec3 translation; glm::vec3 scale; glm::vec3 rotation; }` (`scene.cppm:41`) —
  **rotation is Euler XYZ in radians**, field order translation/scale/rotation. "Move entity" = mutate
  `.translation`.
- No name→entity index; the canonical lookup is a linear `forEach<NameComponent>` scan, **names are not
  unique** (`scene.cppm:1128`, `control_server.cpp:114`); the control plane mirrors this in `resolveEntity`
  (`control_commands_scene.cpp:30`).
- The type-erased component reader: `ComponentTraits.serialize(Scene&, Entity) -> json`, found by name via
  `findByName(reg, name)` (`scene.cppm:908`); the populated registry is `editor->registry`
  (`registerBuiltinComponents`, `host.cppm:426`) — already borrowed as `currentRegistry` in Phase 2.
- The primary camera: `primaryCamera(Scene&)` scans `forEach<TransformComponent, CameraComponent>` and
  takes the first `camera.primary` (`scene.cppm:748`); a camera's pose is its `TransformComponent`
  (`CameraComponent` is projection-only, `scene.cppm:130`).

## Work

### 1. The `ScriptEntity` usertype (extend Phase 2's minimal binding)

Each method `valid`-guards and `hasComponent`-guards, raising `luaL_error` (caught by the per-instance
`pcall`) on misuse. It reads `host.currentScene` / `host.currentRegistry`:

- `entity:valid()` → bool; `entity:name()` → string (`NameComponent`).
- `entity:get_component(name)` → table | nil: `findByName(*currentRegistry, name)`; if present, call
  `traits->serialize(scene, e)` → `nlohmann::json`, convert JSON→Lua table with a small recursive helper
  (objects→tables, arrays→sequence tables, scalars 1:1). A **read-only snapshot** that covers *every*
  registered component with no per-type binding code; returns nil if the entity lacks it.
- `entity:get_position()` → `(x,y,z)`; `entity:set_position(x,y,z)` (from Phase 2).
- `entity:set_rotation(rx,ry,rz)` (**radians**) and `entity:set_scale(sx,sy,sz)` → the matching
  `TransformComponent` fields. Document radians explicitly.

### 2. The `se` module functions

```cpp
luabridge::getGlobalNamespace(L).beginNamespace("se")
    .addFunction("log",                &script_log)                 // Phase 1/2
    .addFunction("get_entity_by_name", &script_get_entity_by_name)  // -> ScriptEntity (invalid if none)
    .addFunction("primary_camera",     &script_primary_camera)      // -> ScriptEntity (invalid if none)
.endNamespace();
```

- `get_entity_by_name(name)`: linear `forEach<NameComponent>` over `*currentScene`, **first** match
  (document first-match + not-unique; this is a deliberate choice). Returns an invalid `ScriptEntity` if
  absent — scripts check `:valid()`. O(N) per call is fine for MVP scene sizes; a glue-owned cache is a
  noted, deferred optimization. With per-entity `self.entity`, most scripts won't need this except to
  reference *other* entities.
- `primary_camera()`: mirror `primaryCamera`'s first-primary scan and return that entity; the script then
  `:set_position(...)` it — that **is** "move camera" (the `renderCameraView`→`primaryCamera`→`renderScene`
  chain picks it up next frame, no render plumbing). Returns invalid if the play scene has no primary
  camera (the renderer falls back to the fly-cam, so it never blacks out).

### 3. Keep the surface tiny

Vectors are plain `(x,y,z)` number tuples in the MVP (a bound `Vec3` usertype is a trivial later add). Do
**not** bind `addComponent`/`destroyEntity`/`setParent` yet (out of MVP scope; `destroyEntity` is recursive,
`setParent` returns a `Result` to surface). `self.entity:parent()`/`:children()` navigation (over
`RelationshipComponent`) is a natural next step, deferred.

## e2e (extend `tests/e2e/script.test.ts`)

A script slot whose `on_update`:
- reads its own `self.entity:get_component("Transform")` and asserts the round-tripped fields (via
  `se.log` or a control-driven check);
- references another entity: `se.get_entity_by_name("Target")` and reads/moves it;
- moves the camera: `se.primary_camera():set_position(0,5,10)`.

Seed the entities, `play`, `settle`, assert via the existing transform-read path (`inspect`/`get-transform`)
that the **play-scene** entities + camera moved, then `stop` and assert the authored scene is unchanged.
Close with the `validationErrors()` case.

## Gate / done

- `make engine` + `make prepare-for-commit` clean; `make e2e` green.
- `scripting.md` gains an **API reference** section: the `se` functions + `ScriptEntity` methods in a table,
  with the radians / first-match / opaque-handle / read-only-snapshot caveats; hub row kept.

## Risks

- `getComponent<C>` is UB if absent; every binding must guard — `set_position` on a transform-less entity
  raises a clean Lua error, never aborts the engine.
- `TransformComponent.rotation` is **Euler radians**, struct order translation/scale/rotation — easy to bind
  wrong; `worldRotation` returns a quat (world vs local differ; MVP exposes local only).
- A script reading `get_component("Transform")` after `set_position` the same frame sees the local value it
  wrote, but **world** matrices refresh in `renderScene` after the tick — expose local transforms only; do
  not surface `worldMatrix` mid-tick.
- `primary_camera()` first-primary pick is entt-iteration-order dependent with multiple primaries; document
  single-primary as supported.
- The JSON→Lua converter must be total over the component DTO shapes (vec3 arrays, uuids as numbers/strings).
