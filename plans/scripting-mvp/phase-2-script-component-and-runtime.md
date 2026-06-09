# Phase 2 — ScriptComponent + per-entity runtime

**Status:** COMPLETED

## Goal

Add the data-only **`ScriptComponent`** (an ordered list of script slots) to `Saffron.Scene`, and the
**per-entity runtime** in `Saffron.Script` that, on **Play**, instantiates each slot's Lua class with
`self.entity` bound to its owning entity and calls `self:on_update(dt)` every frame in **slot order**.
Errors are contained per instance. A minimal `self.entity:set_position` binding ships here so movement is
observable; the full API surface is Phase 3 and editable fields are Phase 4.

## What exists to build on

- **Nested-vector component precedent:** `MaterialSetComponent { std::vector<MaterialSlot> slots; }` with
  the nested `MaterialSlot` struct (`scene.cppm:109,124`) — the exact shape `ScriptComponent`/`ScriptSlot`
  mirror.
- **Component registration + serde (verified mechanism):** components are registered in
  `registerBuiltinComponents` (`scene_edit_components.cpp:19`, called at `host.cppm:426`). The component
  serde in `scene_component_serde.generated.cpp` is **NOT field-derived** — `tools/gen-control-dto/gen.ts`
  parses only `control_dto.cppm` (`gen.ts:39,2061`); the scene serde is a **hand-authored literal block**
  in `emitSceneSerde()` (`gen.ts:1755+`), one `xComponentToJson`/`xComponentFromJson` pair per component
  written by hand. The nested-vector pattern to mirror is `materialSetComponentToJson`/`…FromJson`
  (`gen.ts:1832-1869`). A `Json` (`nlohmann::json`) field round-trips arbitrary JSON natively, so
  `overrides` needs no special handling. Regenerate with `bun run tools/gen-control-dto/gen.ts`.
- **The play duplicate is built by serde** (`sceneToJson`/`sceneFromJson`, `scene_edit_play.cpp:82`), so a
  registered `ScriptComponent` carries into `*ctx.playScene` automatically and is discarded on Stop.
- **The tick + lifecycle seams** (this plan's mechanism): `tickPlay` (`scene_edit_play.cpp:177-197`) and
  `onPlayStateChanged` (`scene_edit_context.cppm:193`); the Host owns the single context + loop
  (`host.cppm:407,525,529,601`).
- **`ScriptVm` / `runFile` / `runString`** from Phase 1; the `Entity` handle + `forEach`/`getComponent`
  facade (`scene.cppm:342,355,424`).

## Work

### 1. The component (`Saffron.Scene`, data-only)

In `scene.cppm`, beside the other components, mirroring `MaterialSlot`/`MaterialSetComponent`:

```cpp
struct ScriptSlot
{
    std::string scriptPath;   // relative to the project src/, e.g. "turret.lua"
    Json overrides;           // per-instance field overrides (empty until Phase 4)
};

struct ScriptComponent
{
    std::vector<ScriptSlot> scripts;   // ordered; runs top-to-bottom
};
```

- Declare the structs in `scene.cppm` (with `overrides` typed as `Json`), and register the component as
  `"Script"` in `registerBuiltinComponents` with its `scriptComponentToJson`/`scriptComponentFromJson`.
- Because the scene serde is hand-authored (see above), **add the serde by hand** in three places in
  `gen.ts`, all mirroring the existing `MaterialSet` component:
  1. the `xComponentToJson`/`xComponentFromJson` pair in `emitSceneSerde()` (mirror
     `materialSetComponentToJson`/`…FromJson`, `gen.ts:1832-1869`); the slot loop pushes each slot, and
     `overrides` round-trips as `{ "overrides", s.overrides }` / `s.overrides = sj.value("overrides",
     nlohmann::json::object())`;
  2. add `"Script"` to the `componentNames` array (`gen.ts:1406`) and hand-write its JSON schema in the
     `schemas` object (`gen.ts:1421+`, mirror `MaterialSet`/`Material`);
  3. add the component to the TS `Components` interface + `ComponentBody` union (`gen.ts:1284,1300`).
- Then run `bun run tools/gen-control-dto/gen.ts`; commit the regenerated
  `scene_component_serde.generated.cpp`, `editor/src/protocol/se-types.ts`, and the schema artifacts.
- The component holds **no Lua state** — only the path + overrides. All instancing lives in
  `Saffron.Script`.

### 2. The decoupling hook + error ring on `SceneEditContext`

As in the simulation seam: add `std::function<void(Scene&, f32)> simTick;` to `SceneEditContext`
(`std`-only — no Lua) and invoke it from `tickPlay`, replacing the `static_cast<void>(dt)` no-op
(`scene_edit_play.cpp:196`):

```cpp
if (ctx.simTick) { ctx.simTick(activeScene(ctx), dt); }
```

Add a bounded session-diagnostics ring (also `std`-only) so the editor can drain script errors via a
normal scene command without Control importing `Saffron.Script`:

```cpp
struct ScriptError { i64 seq; std::optional<u64> entity; std::string script; std::string message; i64 frame; };
std::vector<ScriptError> scriptErrors;
i64 scriptErrorSeq = 0;
```

### 3. The per-entity runtime (`Saffron.Script`)

A script file **returns a class table**; the runtime instances it per slot per entity. Keep one VM with
many instance tables (cheaper than many states), classes cached by path:

```cpp
export namespace se
{
    struct ScriptHost
    {
        ScriptVm vm;
        // path -> ref of the returned class table (LUA_REGISTRYINDEX)
        std::unordered_map<std::string, int> classRefByPath;
        // (entity, slotIndex) -> ref of the instance `self` table
        std::vector<struct ScriptInstance> instances;
        Scene* currentScene = nullptr;            // borrowed; valid only inside scriptTick
        const ComponentRegistry* currentRegistry = nullptr;  // borrowed (used from Phase 3)
    };

    auto startScripts(ScriptHost&, Scene&, std::string_view srcDir) -> Result<void>;
    auto tickScripts(ScriptHost&, Scene&, f32 dt) -> Result<void>;   // returns Err on first instance error
    void stopScripts(ScriptHost&);
}
```

- `startScripts`: `newScriptVm()`. `forEach<ScriptComponent>(scene)` — for each slot **in order**:
  load+run `srcDir/slot.scriptPath` (a missing file is a logged skip, not a fatal error); the chunk must
  return a table (the class). Cache it (`classRefByPath`). Create the instance:
  `self = { entity = <handle> }`, set metatable `{ __index = Class }`, store `selfRef = luaL_ref(...)`,
  record a `ScriptInstance { entity, slotIndex, selfRef, classRef }`. (Phase 4 injects merged fields here;
  call `self:on_create()` if present.)
- `tickScripts`: set `currentScene`/`currentRegistry`; iterate `instances` (so within an entity the slot
  order is preserved; cross-entity order is entt/insertion order, unspecified). For each: push the `msgh`,
  `lua_rawgeti(self)`, fetch `on_update` from `self`, push `self`, push `dt`, `lua_pcall(2, 0, msgh)`;
  on nonzero build `Err(traceback)` (record the entity + script), keep going or early-return per policy.
  Clear the borrows; balance the stack.
- `stopScripts`: call `self:on_destroy()` where present, `luaL_unref` every instance + class ref, clear
  the maps, destroy the VM. `stopPlay` resets `playScene` *before* publishing `Edit`, so teardown must not
  dereference the scene.

### 4. Host wiring (the only edits outside `Saffron.Script`)

In `host.cppm` (the one TU importing both modules):

- Add `ScriptHost script;` to `HostState` (`host.cppm:41-47`).
- In `onCreate`, after `state->editor = newSceneEditContext()` (`host.cppm:407`), subscribe to
  `editor->onPlayStateChanged`: Edit→Playing (no VM yet) → `startScripts(script, activeScene(*editor),
  projectRoot/"src")`; any→Edit → `stopScripts(script)`; pause/resume → no-op (track "VM exists").
- Set `editor->simTick = [state](Scene& s, f32 dt){ auto r = tickScripts(state->script, s, dt); if (!r)
  { /* append to editor->scriptErrors + set a pending-pause flag */ } };`
- **No re-entrant state change:** `simTick` runs inside `tickPlay`, so do not `pausePlay` from within it.
  In the Host `layer.onUpdate`, immediately after `se::tickPlay(...)` (`host.cppm:529`), if a script error
  was recorded this frame, call `se::pausePlay(*editor)` once.
- In `onExit` (`host.cppm:601`), unsubscribe + `stopScripts` **before** `destroySceneEditContext`.

### 5. Minimal binding (enough to observe movement)

Bind the opaque `ScriptEntity` usertype with just `:valid()`, `:get_position() -> x,y,z`, and
`:set_position(x,y,z)` (guarding `valid` + `hasComponent<TransformComponent>`; UB otherwise per
`scene.cppm`). The full surface is Phase 3.

### 6. Control command + `se` parity (keep-current)

Add `get-script-status` and `drain-script-errors` (drain the `SceneEditContext` ring — a normal scene
command, keeping Control free of any `Saffron.Script` import). Declare the DTOs in `control_dto.cppm`
(mirroring `DrainAlarmsParams/Result`, `control_dto.cppm:369-380`), add to `gen.ts` with an `"empty"`
fixture, regenerate, commit. `se` works via generic coercion; a `tools/se` formatter branch is optional.

## e2e (`tests/e2e/script.test.ts`)

Boot `SAFFRON_AUTO_EMPTY_PROJECT`. Author test `.lua` files into the project `src/` and attach a
`ScriptComponent` slot to a seeded entity (via existing scene/component control commands), then `play`:

1. **Move:** an entity `Cube` with a slot whose `on_update` drifts `self.entity` along +X; assert the
   **play-scene** Cube moved, then `stop` and assert the **authored** Cube is unchanged.
2. **Slot order:** two slots on one entity writing distinct axes / appending to a shared global; assert the
   list-order effect.
3. **Contained error:** a slot whose `on_update` calls `error("boom")`; assert `drain-script-errors`
   returns a traceback entry, `get-play-state` is `paused`, the host survives, and the validation log is
   clean. Close with the `validationErrors()` case.

## Gate / done

- `make engine` + `make prepare-for-commit` clean; `make e2e` green; `bun run check` passes.
- `scripting.md` docs page gains the component/slot model, the class-table script shape, the lifecycle, and
  the error model; `## In the code` row for the new command; hub row kept. AGENTS.md DAG table updated.

## Risks

- **entt keys components by type** — multiple scripts per entity *must* be the `scripts` vector, never two
  `ScriptComponent`s. Document this in the API.
- The scene serde is **hand-authored** (`emitSceneSerde()`, `gen.ts:1755+`), not field-derived — adding a
  component means hand-writing its serde + schema + TS in the three `gen.ts` spots above. A `Json`
  `overrides` field is the easy case (nlohmann round-trips it), so **no string-blob fallback is needed**.
- `enterPlay`'s JSON round-trip must serialize `ScriptComponent` cleanly (registered + serde regenerated)
  or scripts won't carry into the play duplicate.
- `onPlayStateChanged` fires for all transitions; the subscriber must distinguish create/destroy/freeze by
  tracking whether a VM exists. `SubscriberList` has no existing subscriber to copy — confirm unsubscribe
  semantics for clean `onExit` teardown.
- Lua method-call stack discipline (`self:on_update`) is fiddly; keep every call `pcall`-wrapped and the
  stack balanced. Never let a Lua error unwind through C++.
- Cross-entity order is entt-iteration order (unspecified, may shift) — by design for now; do not let any
  test depend on it.
- Resolve the project-root field on `SceneEditContext` (set by `applyProjectInfo`,
  `control_commands_asset.cpp:36-44`) before wiring `srcDir`.
