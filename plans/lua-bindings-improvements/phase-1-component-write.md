# Phase 1 — generic component WRITE + transform completeness

**Status:** COMPLETED

The highest value-to-effort phase. `get_component` is read-only today; the registry already carries the
inverse, so a generic **write** is a pure binding with zero per-type code. Plus the missing transform
getters (symmetry + world space), which already have backing functions in `Saffron.Scene`.

This phase ships table-based vectors (`{x,y,z}`); Phase 2 cuts every vector to `sa.Vec3` in one no-legacy
change. Land the **write** API here (stable wire shape), and fold the new world getters' Vec3 form into
Phase 2 so they are born as `Vec3` (Phase 1 → Phase 2 is the locked order, §8 of the README).

## The lever: registry already has the write path

`ComponentTraits` (`scene.cppm:1201`) carries `has`, `addDefault`, `remove`, `serialize`, **and
`deserialize`** (`Result<void>(Scene&, Entity, const json&)`, `:1211`). `get_component` already calls
`traits->serialize` (`script_runtime.cpp:189`); the write side calls `traits->deserialize` / `has` /
`addDefault` / `remove`. Every registered component is reachable with the same type-erased generic code that
backs the read snapshot — **no new C++ in `Saffron.Scene`.**

## New `sa.Entity` methods (all in the `beginClass<ScriptEntity>` block, `script_runtime.cpp:431`)

| Method | Signature | Backing |
|---|---|---|
| `set_component(name, table) -> bool` | `luaToJson(table)` then `findByName(*currentRegistry, name)->deserialize`; `Result` Err → logged warn + `false` | `scene.cppm:1246` |
| `add_component(name) -> bool` | `traits->addDefault` if `!traits->has`; returns whether it was added | `scene.cppm:1236` |
| `remove_component(name) -> bool` | `traits->remove` if present **and** `traits->removable`; returns whether removed | `scene.cppm:1205`/`remove` |
| `has_component(name) -> bool` | `traits->has` | registry |

`set_component` needs a **Lua-table → JSON** converter — the inverse of the existing `jsonToLua`
(`script_runtime.cpp:43`). Add `luaToJson(lua_State*, int index) -> nlohmann::json` in the anonymous
namespace: tables with 1-based integer keys → JSON array, string-keyed tables → object, number/bool/string
scalars 1:1, `nil` → null. In **Phase 2** add an `sa.Vec3` userdata branch (→ a 3-number array). **Reuse for
`set_component`, the Phase 2 Vec3 marshalling, and the Phase 5 bridges' arg marshalling.** Partial patches
work for free: `deserialize` merges onto the existing component (the same path the control-plane
`set-component` command uses, `control_commands_scene.cpp` — mirror its partial-JSON-patch behavior).

Guard exactly like `getComponentSnapshot`: `host->currentScene`/`currentRegistry` non-null, entity valid,
name known (`findByName`, `scene.cppm:1272`) — else a logged no-op (the `transformScene`/`logWarn` idiom,
`script_runtime.cpp:95`). A write to an unknown component name is a logged skip, never an error (matches
`get_component` returning nil).

### The structural-component gate (LOCKED — keyed on the registered NAME string, in the binding)

`deserialize` **auto-adds the component if absent** (`scene.cppm:1248`, `if (!hasComponent<C>) addComponent<C>`).
That is correct for scene-load but dangerous for cache/asset-backed components mid-play. So `set_component`
and `add_component` carry an in-binding **deny list keyed on the registered name string** (NOT in
`ComponentTraits` — `traits->deserialize` is shared with scene-load, where the auto-add is wanted):

```cpp
// Refuse a write that would desync the live Jolt world / hierarchy / rig caches cooked at play start.
static constexpr std::array kStructuralComponents = {
    "Relationship", "SkinnedMesh", "Bone", "FootIk", "BonePhysics",
    "Collider", "Rigidbody", "KinematicBones",
};
```

These are every cache/asset-backed structural component registered at `scene_edit_components.cpp:102`–`138`
(verify the exact strings against that file before hard-coding). A `set_component`/`add_component` naming one
is a logged warn + `false`. **Hierarchy is mutated only via `set_parent`** (Phase 3, which relinks); physics
shape/body changes are not script-writable in v1. `CharacterController`, `AnimationPlayer`, `Transform`,
lights, camera, material — all allowed (value/round-tripping serde).

> **Note (no-legacy + the move-character gap):** `CharacterController` is *allowed* by this gate, but a
> `set_component("CharacterController", { desiredVelocity = … })` is silently a **no-op** for the velocity
> fields — `characterControllerComponentFromJson` resets `desiredVelocity`/`verticalVelocity`/`onGround` to
> zero on every deserialize and `…ToJson` omits them (`scene_component_serde.generated.cpp:669`–`687`). That
> is *why* Phase 5 makes `move_character` a **required** named bridge, not redundant sugar. Document this so
> no one "simplifies" the bridge into a generic `set_component` write that does nothing.

## Transform completeness (`script_runtime.cpp`, `ScriptEntity`)

`getPosition`/`setPosition`/`setRotation`/`setScale` exist; the symmetric getters and world-space variants
are missing but fully backed in `Saffron.Scene`:

| New method | Backing (`scene.cppm`) | Notes |
|---|---|---|
| `get_rotation()` | `TransformComponent.rotation` | local Euler radians (mirror `get_position`) |
| `get_scale()` | `TransformComponent.scale` | local |
| `get_world_position()` | `worldTranslation(scene, entity)` (`:891`) | composed world |
| `get_world_rotation()` | `worldRotation(scene, entity)` (`:897`) + `quatToEulerZYX` (`:977`) | quat → Euler radians (matches `set_rotation`) |

These return **`{x,y,z}` tables in Phase 1** (parity with today's `get_position`); **Phase 2's no-legacy
cutover replaces the new and the old vector returns in one stroke** with `sa.Vec3`. `get_world_matrix`
(`worldMatrix`, `:882`) is deferred unless a script needs the full mat4. `look_at` is a Phase 2 math binding
(needs the quat→euler decomposition).

All getters/setters keep the `transformScene` guard — a stale/dead handle is a logged no-op.

## Control command (drivable-state rule)

Component write is already drivable from the editor via the existing `set-component` / `add-component` /
`remove-component` control commands (the e2e harness's `attachScripts` uses `add-component` +
`set-component`). **No new command needed** — this phase exposes the existing capability to Lua, it does not
add new engine state.

## Tests (`tests/e2e/script.test.ts`)

- A script `add_component("PointLight")` then `set_component("PointLight", { intensity = 5 })`; after a tick
  `inspect` shows the component present with the patched value.
- `has_component("Transform")` true, `has_component("Nope")` false; `remove_component` on a present component
  → re-`has` false; `remove_component` on a non-removable component → `false`, still present.
- `set_component` on an unknown name leaves play `playing` (logged skip).
- A partial patch (`set_component("Transform", { translation = {x=9} })`) changes only `x` (merge semantics).
- The gate: `set_component("Rigidbody", …)` / `add_component("Collider")` returns `false` and leaves play
  `playing` (logged refusal), the live component unchanged.

## Docs

Update `docs/content/explanations/scripting/script-components-and-runtime.md`'s `se`/entity API reference
table with the four component-write methods + the transform getters + the structural-component gate note;
update the `scripting/_index.md` hub row if the reference's scope line changes.

## Constraints honored

NO-LEGACY (no duplicate write path; the gate prevents a desyncing second mutation route), Saffron.Script
imports only Core+Scene (no new import — registry traits are in Scene), sandbox unchanged, errors as
logged-warn/`false` (no exceptions). Generated serde files are never hand-edited (the write rides
`traits->deserialize`, not a per-type setter).

## Verification gate

`make engine` clean, `make prepare-for-commit` clean, `make e2e` green (the new script cases), and the
contract test still passes (no command surface changed).
