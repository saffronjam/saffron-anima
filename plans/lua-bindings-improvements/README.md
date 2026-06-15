# Lua binding surface expansion — design

**Status:** COMPLETED — Phases 1–6 shipped in full; Phase 7 shipped as 7a (hand-written
`library/se.lua` + `.luarc.json` scaffold + the gating drift tripwire). **7b (the declarative
binding-table + generator cutover) was deliberately not done** — LuaBridge registers each function by
its deduced C++ type, so a single iterating table forces all ~60 bindings into raw `lua_CFunction`
thunks with manual stack marshalling, strictly worse than today's member-pointers + small lambdas,
for a single-source benefit the gating tripwire already delivers. The hand-written `se.lua` + tripwire
is one coherent approach (no superseded path lingers), so the no-legacy stance holds. See
`phase-7-luals-defs-and-codegen.md` for the full decision record.

The authoritative, decision-locked design for scaling the `Saffron.Script` Lua surface so a script can
drive **full engine functionality as Saffron stands today**, plus a LuaLS definition file for VS Code
autocomplete. Grounded in the SOTA scripting layers (Unity, Unreal, Godot, Defold, Roblox/Luau, LÖVE) and
the binding-idiom literature (sol2 / LuaBridge3), and fitted to the repo's hard constraints: no legacy, no
`Saffron.Physics`/`Saffron.Animation` import in `Saffron.Script`, the host-callback POD bridge
(`ScriptHost::raycast`), the Lua sandbox, errors-as-`Result`, and the one-source-of-truth codegen stance.

This is **a plan only.** No engine/editor source is touched here. The per-phase files (`phase-1-*` …
`phase-7-*`) carry the step-by-step edits and their own `**Status:**` lines; this README is the locked
design they implement. `phase-0-research.md` is the SOTA + decision-record grounding.

---

## 1. North star + principles

**Target.** A scripter, working in VS Code with autocomplete, can write per-entity gameplay against the
whole of today's engine: read and **write** any component, move and spawn entities, query and push physics,
read input edges and the mouse, drive animation clips, run a follow camera, schedule work with `se.wait`,
and message other scripts — all in a sandboxed Lua 5.5 VM that degrades safely and never crashes.

**Principles (locked).**

1. **Registry-driven, not per-type.** The single biggest lever is that `ComponentTraits` already carries
   `serialize` / `deserialize` / `has` / `addDefault` / `remove` (`scene.cppm:1201`–`1214`), synthesized
   once per registered component by `registerComponent` (`scene.cppm:1229`). `get_component` already rides
   `serialize`; the **write** half is the exact mirror with **zero per-type code**. Every component
   capability flows through this one generic seam — never a `set_light` / `set_camera` per-component setter.
2. **One cross-module pattern.** `Saffron.Script` imports only `Saffron.Core` + `Saffron.Scene`
   (`script.cppm:29`–`30`). Physics/animation reach is **always** a host-bound `std::function` over a
   Jolt-free / glm-light POD — the `ScriptHost::raycast` template (`script.cppm:107`, wired at
   `host.cppm:1199`). No phase ever adds an `import Saffron.Physics`.
3. **Degrade-to-no-op handles.** `se.Entity` is a value handle `{ entt id, ScriptHost* }`; it reaches the
   scene only through `host->currentScene`, non-null only while a start/tick/stop/contact call is on the
   stack, and every op routes through `transformScene` (`script_runtime.cpp:95`) — a stale or dead handle
   is a **logged no-op**, never a deref. Every new method keeps this guard.
4. **No legacy.** When a shape is superseded (the `{x,y,z}` tables → `se.Vec3`), it is **replaced**
   wholesale in one change — callers, scaffold, docs, e2e — never run beside the old shape. When a new name
   would duplicate an existing binding's purpose, the **existing** one is renamed in place, not joined by a
   second.
5. **Sandbox stays.** base/coroutine/string/math/table/utf8 only (`script.cppm:237`); the timer scheduler
   is built on the already-enabled `coroutine` lib, never `os.clock`.
6. **Keep-current is part of done.** Each phase ships with its `docs/content/explanations/scripting/` page
   (+ `_index.md` row), its `tests/e2e/script.test.ts` cases (the clean-log assertion stays green), and a
   control command **where it adds drivable/inspectable state**.

**v1 (this plan) vs deferred.**

| In v1 | Deferred, with reason |
|---|---|
| Generic component **write** (`set/add/remove/has`) | **Field-path** writes `set("Light.intensity", v)` — whole-component table matches the serde grain; add sub-paths later only if a real need appears (one access model first). |
| `se.Vec3` userdata replacing all `{x,y,z}` tables | `se.Quat` userdata — rotation is Euler radians in `TransformComponent.rotation`; a `look_at` helper covers the camera case. Add Quat only when gimbal-free composition is needed. |
| Transform completeness (local + world **getters**) | World-space **setters** — need an inverse-parent solve; ship world getters now, defer setters with this reason. |
| spawn / destroy / set_parent / find-by-name (renamed) | **find-by-tag** — there is no `TagComponent` (only `NameComponent`); a tag lookup needs new C++. Defer; ship find-by-name now. |
| Key **edge** detection + **mouse** | **Gamepad** — `window.cppm:29`–`33` has no gamepad signal anywhere; needs new SDL plumbing. Out of scope; do **not** annotate it. |
| Physics bridges (shapecast, ragdoll, move-character) + **new** impulse/velocity C++ | An Enhanced-Input-style **action map** — a data-asset remap layer; fits the registry philosophy later. Bind raw input now. |
| Animation play/stop/seek via `set_component` | An animation **event** channel (clip-finished → handler) — needs a runtime hook; defer. A thin `play_clip` sugar bridge is also deferred (it would duplicate the `set_component` field write — no-legacy). |
| `se.broadcast` / `entity:send` bus + `se.wait`/scheduler | Roblox `GetPropertyChangedSignal` — no registry change-hook; scripts poll or fire explicit messages. |
| Hand-written `se.lua` + runtime tripwire | The declarative binding table + generator (Phase 7b) — lifts once the surface stabilizes. |

**Lifecycle is NOT expanded.** The handler set (`on_create` / `on_update(self, dt)` [required] /
`on_destroy` / `on_trigger_enter/exit` / `on_contact`) is correct as-is. Saffron runs physics then scripts
in one `simTick` (`host.cppm:1142`–`1189`) at the sim dt, so `on_update` already **is** the physics-settled
("FixedUpdate-equivalent") hook. **No second `on_fixed_update` channel** — that would be the
duplicate-path no-legacy forbids; the engine has one tick.

---

## 2. The type + handle model

### Handle model — LOCKED, unchanged

Keep `se.Entity` exactly as it is: a trivially-copyable value handle, scene reached only via
`host->currentScene`, every mutating op gated by `transformScene` → logged no-op on a stale/dead handle.
`:valid()` is the explicit guard (the Unity `== null` / Roblox `is_instance_valid` analog). **Every new
method (write, spawn, destroy, set_parent, bridges) routes through the same guard.** Cross-entity refs are
resolved by **uuid** (`findEntityByUuid`, `scene.cppm:735`), never by stashing a raw `entt` id that a
destroy could invalidate.

### Vec3 — LOCKED: a real `se.Vec3` userdata, replacing tables wholesale

**Decision: introduce an `se.Vec3` userdata backed by `glm::vec3`, and cut every `{x,y,z}` table over to it
in one change.** Justification, weighing all three axes:

- **Ergonomics** — operators read naturally (`pos + dir * speed * dt`), the single biggest readability win;
  it is what every SOTA layer ships (Roblox `Vector3`, Godot `Vector3`, Defold `vmath.vector3`, Unreal
  `FVector`). Tables give none of this.
- **Per-frame GC** — a `sizeof(glm::vec3)` ≈ 12-byte trivially-copyable value userdata is **cheaper** than
  today's `lua_createtable` + 3 `settable` (no field-name hashing, smaller block), and the GC reclaims in
  batches. The current table return is the documented anti-pattern, not the cheap path.
- **Migration cost** — real but contained: `getPosition` (`script_runtime.cpp:116`), `pushVec3Table`
  (`:255`, contact point/normal, used at `:287`–`:288`), the raycast result `point`/`normal`, and the
  `vec3` property inference in `inferField` (`:699`, `:720`) + per-instance copy in `injectFields`
  (`:359`)/`pushTableCopy` (`:340`). All cut in the same change (Phase 2) — never an additive second
  representation.

**Registration shape — LOCKED** (`script_runtime.cpp`, inside the `se` namespace). Note the **read-write**
property form and the **dual `__mul` overloads**:

```cpp
.beginClass<glm::vec3>("Vec3")
    .addPropertyReadWrite("x", &glm::vec3::x)   // read-WRITE: single-arg addProperty is read-only
    .addPropertyReadWrite("y", &glm::vec3::y)   // (Namespace.h:886 → push_property_readonly); the
    .addPropertyReadWrite("z", &glm::vec3::z)   // ReadWrite form (Namespace.h:920) is required so v.x = 5 works
    .addStaticFunction("new", +[](f32 x, f32 y, f32 z) { return glm::vec3{ x, y, z }; })
    .addFunction("__add", +[](const glm::vec3& a, const glm::vec3& b) { return a + b; })
    .addFunction("__sub", +[](const glm::vec3& a, const glm::vec3& b) { return a - b; })
    .addFunction("__mul",                                   // overload set: BOTH operand orders
        +[](const glm::vec3& a, f32 s) { return a * s; },   //   vec * scalar
        +[](f32 s, const glm::vec3& a) { return a * s; })   //   scalar * vec (Lua dispatches __mul on the
                                                            //   right operand when the left is a number)
    .addFunction("__unm", +[](const glm::vec3& a) { return -a; })
    .addFunction("__eq",  +[](const glm::vec3& a, const glm::vec3& b) { return a == b; })
    .addFunction("__tostring", +[](const glm::vec3& v) { return std::format("Vec3({},{},{})", v.x, v.y, v.z); })
    .addFunction("length",     +[](const glm::vec3& v) { return glm::length(v); })
    .addFunction("normalized", +[](const glm::vec3& v) { return glm::normalize(v); })
    .addFunction("dot",   +[](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); })
    .addFunction("cross", +[](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); })
    .addFunction("lerp",  +[](const glm::vec3& a, const glm::vec3& b, f32 t) { return glm::mix(a, b, t); })
.endClass()
```

Plus free helpers `se.vec3(x,y,z)` (factory alias), `se.lerp(a,b,t)`, `se.look_at(eye, target, up?) ->
glm::vec3` (Euler radians via `glm::quatLookAt` then `quatToEulerZYX`, `scene.cppm:977`, so it feeds
`set_rotation`).

**Property-binding caveat (note in Phase 2):** `&glm::vec3::x` relies on GLM keeping `x`/`y`/`z` as
addressable named members. Under the engine's config (`cmake/Dependencies.cmake` sets neither
`GLM_FORCE_XYZW_ONLY` nor `GLM_FORCE_SWIZZLE`, so swizzle is **disabled**) `vec3` uses the anonymous-union
layout `union { T x, r, s; }` and `&glm::vec3::x` is well-formed; it also holds under `XYZW_ONLY`. A future
GLM flag flip would break this — Phase 2 carries the one-line note so it is caught.

**Interop / getters / setters / properties:**

- `get_position`, `get_world_position`, raycast `point`/`normal`, contact `point`/`normal` **return `Vec3`
  userdata** (one push seam replacing four hand-built tables).
- Setters take **one `Vec3`**: `set_position(v)`, `set_rotation(v)`, `set_scale(v)`. No-legacy: the
  three-float overloads are removed, not kept beside the Vec3 form. **This setter-signature change** (not
  just the getter return) is what forces every e2e `set_position(x, y, z)` call (`script.test.ts:51`–`52`,
  `:62`–`63`, `:73`, `:82`–`83`, `:109`, `:123`–`124`, `:136`, `:159`–`160`) to become
  `set_position(se.vec3(...))` / `set_position(p + …)` in the same change, and the scaffold orbit math +
  its `Math.hypot` assertion (`script.test.ts:416`) to rebuild `self.center` from `Vec3`, not `{x=…}`.
- A `vec3` **declared field** default is written `offset = se.vec3(0, 1, 0)`. So **`se.Vec3` must be
  registered in `newScriptVm` too** (`script.cppm:230`–`246`, today only `se.log`), not only `startScripts`
  (`script_runtime.cpp:429`), because `readScriptSchema` runs the class chunk in a throwaway `newScriptVm`
  (`script_runtime.cpp:745`).
- `inferField` (`:699`) must **detect** a `Vec3` **userdata** default (its metatable/class), not
  `lua_rawlen == 3` (`:720`), **and switch its extraction**: the 3-number JSON array must be built from the
  userdata's `x`/`y`/`z` (LuaBridge getters or a direct cast), **not** `lua_rawgeti`, or the array silently
  emits zeros. The wire shape stays the 3-number array the Inspector + override storage expect
  (`ScriptField.defaultValue`).
- `injectFields` constructs a **fresh `Vec3` per instance** by value-pushing the registered value type. The
  `pushTableCopy` table-only branch (`:378`, `lua_type == LUA_TTABLE`) no longer fires for a `Vec3` default
  (it is userdata now); rewire the branch so a userdata default is value-pushed per instance (no shared
  reference), preserving the no-shared-default-aliasing guarantee `pushTableCopy` exists for.

---

## 3. The entity + component API

### Component access — LOCKED: generic whole-component get/set/add/remove/has

**Decision: a single generic, registry-driven access path keyed by component name, taking/returning a whole
table** — Defold's `go.get/go.set(url, …)` realized over the type-erased registry, **not** Unity/Roblox-style
typed per-component proxies (which would duplicate the serde and drift). All four writes are **pure
bindings** mirroring the existing read:

| Method | Backing (`scene.cppm`) | Notes |
|---|---|---|
| `entity:get_component(name) -> table\|nil` | `traits->serialize` (exists) | unchanged |
| `entity:set_component(name, table)` | `traits->deserialize` (`:1246`) | `luaToJson(table)` then `deserialize`; `Result` Err → logged warn + `false` |
| `entity:add_component(name)` | `traits->addDefault` (`:1236`) | |
| `entity:remove_component(name) -> bool` | `traits->remove`, gated on `traits->removable` (`:1205`) | |
| `entity:has_component(name) -> bool` | `traits->has` | |

The only new helper is **`luaToJson(lua_State*, int)`**, the total inverse of the existing `jsonToLua`
(`script_runtime.cpp:43`): Lua tables → JSON object/array by key shape, scalars 1:1, `Vec3` userdata → a
3-number array. Each call resolves traits via `findByName(*host->currentRegistry, name)` (`:1272`) and is
guarded exactly like `getComponentSnapshot` (`:189`).

**This single lever drives, with no new C++:** `AnimationPlayerComponent` (`clip`/`time`/`speed`/`playing`/
`wrap` — the exact fields the play-animation control command writes, `control_commands_animation.cpp:264`),
lights, camera params, material refs — every registered component **whose serde round-trips its fields**.
**It does NOT reach `CharacterControllerComponent.desiredVelocity`/`verticalVelocity`:**
`characterControllerComponentFromJson` resets them to zero on every deserialize and `…ToJson` deliberately
omits them (`scene_component_serde.generated.cpp:669`–`687`, comment "the runtime velocity/ground state
serialize as their defaults — move-character writes them at play time"). So a `set_component` write to
those fields is silently a **no-op** — which is exactly why §4 makes `move_character` a **required** named
bridge, not ergonomic sugar.

**Mid-tick safety + `sceneVersion`.** `deserialize` runs the **same path** scene-load uses; on play, the
live scene is the throwaway duplicate, so a Lua write mutates that duplicate (correct — it is discarded on
stop). Two hazards, locked:

- **`deserialize` auto-adds the component if absent** (`scene.cppm:1248`, `if (!hasComponent<C>) addComponent<C>`).
  Fine for value components; dangerous for **structural / cache-backed** ones. A **name allow/deny gate
  (LOCKED), keyed on the registered name string** (not the C++ type) and living **in the `set_component` /
  `add_component` binding** (NOT in `ComponentTraits` — `traits->deserialize` is shared with scene-load,
  where the auto-add is wanted): `set_component` / `add_component` **refuse** the registered names
  `"Relationship"`, `"SkinnedMesh"`, `"Bone"`, `"FootIk"`, `"BonePhysics"`, `"Collider"`, `"Rigidbody"`,
  `"KinematicBones"` (logged warn + `false`) — every cache/asset-backed structural component registered at
  `scene_edit_components.cpp:102`–`138`. Writing them mid-play desyncs the live Jolt world / hierarchy /
  rig caches that `populatePhysicsWorld` / `relinkHierarchy` cooked at play start. Hierarchy is changed
  **only** through `set_parent` (which relinks); physics shape/body changes are **not** script-writable in
  v1 (documented). `CharacterController` (allowed — moved via the §4 bridge, not via these fields),
  `AnimationPlayer`, `Transform`, lights, camera, material — all allowed.
- **`sceneVersion`** is the control-plane diff-poll signal bumped by editor ops
  (`scene_edit_context.cppm:211`). A script write during play mutates the play duplicate, which the editor
  does **not** diff-poll during play; on stop the duplicate is discarded and `sceneVersion` already bumps
  (`scene_edit_play.cpp:158`). So **no extra bump is needed for play-time writes**, and the plan does not
  add one (it would be churn). Documented explicitly.

### Transform completeness — LOCKED, all pure bindings

| Method | Backing | Tag |
|---|---|---|
| `get_position()` (local) | `TransformComponent.translation` | exists, → `Vec3` |
| `get_rotation()` (local Euler rad) | `TransformComponent.rotation` | **pure binding** (symmetry) |
| `get_scale()` (local) | `TransformComponent.scale` | **pure binding** |
| `get_world_position()` | `worldTranslation` (`scene.cppm:891`) | **pure binding** |
| `get_world_rotation()` (Euler rad) | `worldRotation` (`:897`) + `quatToEulerZYX` (`:977`) | **pure binding** |
| `set_position/rotation/scale(Vec3)` (local) | existing setters, Vec3-cut | exists |
| `se.look_at(eye, target, up?)` | glm + `quatToEulerZYX` | **pure binding** (math) |

World **setters** are deferred (inverse-parent solve). All getters/setters keep the `transformScene` guard.

### Entity lifecycle — LOCKED, pure bindings (deferred-flush)

| API | Backing | Tag |
|---|---|---|
| `se.spawn(name) -> se.Entity` | `createEntity` (`scene.cppm:618`) | **pure binding** |
| `entity:destroy()` | `destroyEntity` (`scene.cppm:630`) | **pure binding** |
| `entity:set_parent(other)` / detach | `setParent` (`:1009`, relinks `:1069`, guards self/cycle) | **pure binding** |
| `entity:parent() -> se.Entity\|nil` / `entity:children() -> {se.Entity}` | `RelationshipComponent` caches | **pure binding** (read walk) |
| `se.get_entity_by_name(name) -> se.Entity\|nil` | `forEach<NameComponent>` (the existing binding, `:451`) | **pure binding** (unchanged name) |
| `se.find_all_by_name(name) -> {se.Entity}` | `forEach<NameComponent>` (multi-match) | **pure binding** (new, distinct purpose) |
| `se.find_by_uuid(uuid) -> se.Entity\|nil` | `findEntityByUuid` (`:735`) | **pure binding** |

**Name discipline (no-legacy).** The existing first-match lookup stays named `se.get_entity_by_name`
(`script_runtime.cpp:451`); the plan does **not** introduce a second `se.find_by_name` for the same
`NameComponent` lookup. The genuinely new surface is the **multi-match** `se.find_all_by_name` (a distinct
purpose the existing binding can't serve) and the uuid lookup. The def file, docs, and e2e use the one
`get_entity_by_name` name; only add `find_all_by_name`/`find_by_uuid` beside it.

**Reentrancy — LOCKED.** `tickScripts` iterates the **live** `host.instances` vector by reference
(`script_runtime.cpp:583`; same direct-reference loop in `startScripts:562`, `dispatchContact:628`,
`stopScripts:662` — there is no pre-existing snapshot). So mutating the container or `entt` storage
mid-loop is unsafe, and **spawn / destroy / set_parent during a tick are queued and flushed after the
instance loop** (Godot `queue_free` discipline) — the deferred flush, not a snapshot, is what makes them
safe. A single `relinkHierarchy` runs at flush end via a `host.hierarchyDirty` flag (not per-op;
`createEntity` does **not** relink, `setParent` does — the flag normalizes this). **Self-destroy is deferred
to flush** so the handle stays valid for the rest of the current handler, then `:valid()` flips false.
**Spawned entities are play-duplicate-scoped, vanish on stop, and their `ScriptComponent` does not
instantiate until the next play** — documented loudly (matches Roblox `Instance.new` in a running game being
transient).

---

## 4. Physics / character / input / camera / animation

Each row tagged **bridge** (host `std::function` over existing C++, the raycast pattern) or **NEW C++**
(engine function must land first). All bridges wired in `host.cppm` next to `state->script.raycast`
(`:1199`), guarded on `state->physics.has_value()` (`:1202`); an unset bridge is a logged-miss no-op.

### Physics

| Lua API | Backing | Tag |
|---|---|---|
| `se.raycast(...)` | `raycastWorld` (`physics.cppm:135`) | exists |
| `se.spherecast(origin, dir, radius, maxDist) -> RayHit` | `sphereCastWorld` (`physics.cppm:139`) | **bridge** — `ScriptHost::sphereCast`, reuses `ScriptRayHit` POD verbatim |
| `entity:move_character(velocity, jump?)` | writes `CharacterControllerComponent.desiredVelocity` / `verticalVelocity` (the `move-character` command body, `control_commands_physics.cpp:224`/`:227`) | **REQUIRED bridge** — `set_component` cannot reach these fields (serde resets/omits them); `jump?` → the existing fixed `verticalVelocity = 5.0f` jump impulse |
| `entity:enable_ragdoll(bool)` / `entity:set_ragdoll_blend(active, bodyWeight?, …)` / `entity:ragdoll_state()` | `enableRagdoll`/`disableRagdoll`/`setRagdollBlend`/`ragdollState` (`physics.cppm:145`–`193`) | **bridge** — `ScriptHost::setRagdollBlend` etc., small bool/f32 PODs |
| `entity:apply_impulse(Vec3)` / `entity:set_velocity(Vec3)` / `entity:add_force(Vec3)` / `entity:get_velocity() -> Vec3` | **none — no force/impulse/velocity API exists** (`physics.cppm` exports none; `BodyInterface` is internal-only; the only `SetLinearVelocity` is on the `CharacterVirtual` inside `stepPhysics`, `physics.cpp:929`) | **NEW C++** — see §7.1 sub-task |

`move_character` is the **one** named bridge that is **required** rather than ergonomic sugar: the generic
`set_component` path is silently a no-op for the velocity fields (§3), so this is not a redundant second
write path — it is the *only* path. The `jump?` boolean maps to the engine's existing fixed
`verticalVelocity = 5.0f` jump (not an arbitrary value), preserving the `move-character` command's behavior.
No other per-component sugar bridge is added — that would be the redundant second path no-legacy forbids.

### Input — held works, edges + mouse are NEW host plumbing, gamepad out of scope

`se.is_key_pressed(key)` reads `ScriptHost::inputKeys` (`script.cppm:104`), a held-key set **forwarded from
the editor over the control plane** (`script-input` command, `control_commands_scene.cpp:1700`), not from
SDL — this is a headless host. So:

| Lua API | What it needs | Tag |
|---|---|---|
| `se.is_key_pressed(key)` (held) | — | exists |
| `se.just_pressed(key)` / `se.just_released(key)` (edge) | host diffs this-tick vs last-tick held sets into two derived sets pushed alongside `inputKeys`; the editor already sends held keys | **NEW C++** (host plumbing — small) |
| `se.mouse_position() -> Vec3` / `se.mouse_delta() -> Vec3` / `se.mouse_button(n) -> bool` / `se.mouse_scroll() -> number` | the `script-input` DTO (`control_dto.cppm`, today only `keys`) + `ScriptHost` input state widened to carry mouse; the editor forwards it (the engine window has no mouse signal, `window.cppm:29`–`33`) | **NEW C++** (DTO + editor + host) |
| gamepad | nothing exists anywhere | **DEFERRED** — not annotated, not bound |

**LOCKED: one input source of truth — the control-plane `script-input` channel.** Edges are derived
**host-side** from the held diff (a key that flips between two snapshots is missed — acceptable, documented).
This keeps headless e2e drivable (the test seam stays the `script-input` command) and avoids a second
SDL-capture feed. Engine-window SDL capture is explicitly **not** pursued in v1.

### Camera — a Lua recipe, no C++ helper

No camera C++ helper is added. A follow camera is a **documented Lua recipe** composed from the new
bindings: a separate camera entity whose script `lerp`s its world position toward
`target:get_world_position() + offset`, orients with `se.look_at`, and uses `se.raycast` for occlusion
pull-in. Ships as the recipe in the camera/scripting docs page and optionally a scaffold example — not an
engine API (keeps the surface minimal, showcases the new bindings). Per-camera params (fov/near/far) ride
generic `set_component("Camera", …)`.

### Animation — `set_component`, no bridge

Clip control is **field writes on the registered `AnimationPlayerComponent`** (the play/seek/loop/playing
control commands are exactly that, `control_commands_animation.cpp:264`), and those fields **do** round-trip
through serde (`scene_component_serde.generated.cpp:299`–`313`), so it rides generic `set_component` with
**no animation import and no new bridge**. v1 binds the minimal play/stop as
`entity:set_component("AnimationPlayer", { clip = uuid, playing = true })` (the script supplies the clip
uuid). A thin `entity:play_clip(uuid)` sugar is **deferred** (it would duplicate the `set_component` field
write — no-legacy). Contact callbacks (`on_contact`/`on_trigger_*`) already work via the contact ring →
`dispatchContact` — **kept unchanged.**

---

## 5. Events + coroutines

Both are **pure Lua** on the already-enabled `coroutine` lib + the single shared VM (all instances share one
`lua_State`, `script.cppm:93`/`:99`, so messaging is in-process). **No new module and no cross-module
import** — but they do add new **`ScriptHost` host-struct state** (a message queue + a coroutine scheduler)
and new **`tickScripts` flush/pump code** (see §7.5); it is not "no new C++", it is "no new module".

**Messaging — LOCKED: one entity-targeted bus, Defold/Unity `SendMessage`-shaped.** Exactly one mechanism
(no UnityEvent-style registry beside it):

- `entity:send(handler_name, ...)` — queue `{target, name, args}` during the tick; **flush after the
  instance loop** (same reentrancy discipline as spawn/destroy) by invoking each matching instance's
  `self:<handler_name>(sender, ...)` via the existing `callInstanceMethod` machinery.
- `se.broadcast(handler_name, ...)` — the global variant over every instance.
- Each dispatched call runs under the existing `pcall` + traceback (pause-on-error containment); one bad
  handler does not abort the rest or crash the VM.

**Timers / coroutines — LOCKED: a `task`-style scheduler over `coroutine` + the tick dt.** Injected as a
Lua prelude at VM creation; pumped from `tickScripts` (resume due coroutines **inside the tick window**, so
`currentScene` is bound and handle ops are not silent no-ops):

- `se.wait(seconds)` — yields the current coroutine; the runtime resumes it once accumulated `dt` passes the
  deadline. Logged error if called outside a coroutine.
- `se.spawn_task(fn, ...)` — start a managed coroutine, resumed immediately (Roblox `task.spawn`).
- `se.delay(seconds, fn)` — the callback form (Defold `timer.delay` / Roblox `task.delay`).

**Never `os.clock` / no busy-wait** (sandboxed out, single VM). A faulting coroutine is contained like a
handler (pause-on-error → script-error ring). The resume site being **inside** the tick window is a hard
requirement (a resume with `currentScene` null would make handle ops mysteriously no-op).

---

## 6. The `se.lua` def file + `.luarc.json` + scaffold

### The fork — LOCKED: hand-write `se.lua` + a runtime tripwire NOW; declarative table + generator in Phase 7b

**Decision and justification.** A generator **cannot** read the imperative `.addFunction(...)` fluent chain
(`script_runtime.cpp:429`–`523`) the way `gen.ts` reads flat structs in `control_dto.cppm` — names live in
call arguments and lambda bodies (and `se.log` is bound in a **second** TU, `newScriptVm`, `script.cppm:238`).
A pure source-parse would be brittle and silently wrong. So:

1. **Phases 1–6 hand-write `library/se.lua`** (`---@meta`, types only) **plus a runtime-introspection
   tripwire** wired as a **gating step** in `tools/ci/check.sh` (next to the DTO `git diff --exit-code`
   guard at `check.sh:24`, inside the `… || fail=1` block): boot a sandboxed probe VM, enumerate the live
   `se` global table **and** the `se.Entity` metatable for every exposed name (covering **both**
   registration TUs — `newScriptVm`'s `se.log`/`se.Vec3` and `startScripts`'s Entity/raycast block), parse
   `se.lua`'s `---@field`/`---@class`/method names, and **fail if the live surface has a name `se.lua`
   lacks** (warn on the reverse). ~30 lines, no binder rewrite, kills the only failure mode that matters (a
   binding with no annotation). This honors the repo's no-drift stance — and the repo already blesses the
   "hand-maintained body, guarded artifact" pattern (`emitSceneSerde` is hand-written under a generated
   header, `gen-control-dto/AGENTS.md:29`).
2. **Phase 7b lifts** the binding list into **one declarative C++ table** that both the LuaBridge
   registration loop **and** an `se.lua` generator consume (the `gen.ts` one-source-of-truth model). The
   tripwire then becomes a "generated `se.lua` is byte-fresh" `git diff` check identical to the DTO gate,
   and the hand-written `se.lua` is **deleted** (no-legacy — the generated file replaces it). Deferred to
   7b because the high-value pure bindings must not wait on a binder rewrite, and the schema is easier to
   design once the real surface exists.

**Pure hand-write-and-hope is rejected** (it drifts invisibly — autocomplete just lacks an entry, no test
fails). The tripwire is mandatory and gating, not optional.

### `se.lua` content shape (`---@meta`)

One file describing the whole surface, with a header comment noting the **LuaLS 5.4 vs runtime 5.5** target
gap (LuaLS has no 5.5 target; the surface uses no 5.5-only syntax). The full surface is reproduced in
`phase-7-luals-defs-and-codegen.md`; in summary it covers `se.Vec3` (operators via `---@operator`),
`se.RayHit`, `se.Entity` (every method from Phases 1–6), `se.ScriptSelf` (the `self` shape), and the `se`
globals. Each entry is annotated **only when its binding is live**; the tripwire enforces the match.

### `.luarc.json` (LOCKED)

```json
{
  "runtime.version": "Lua 5.4",
  "workspace.library": ["library"],
  "diagnostics.globals": ["se"],
  "runtime.builtin": { "io": "disable", "os": "disable", "debug": "disable", "package": "disable" }
}
```

`Lua 5.4` because LuaLS has no 5.5 target; `runtime.builtin` disables exactly the sandboxed-out libs
(matching `luaL_openselectedlibs`, `script.cppm:237`, which omits io/os/debug/package) so the IDE flags
forbidden calls. Both files written **only when absent** (never clobber a user-edited copy).

### Scaffold injection (`assets.cppm`) — LOCKED

- New `ensureScriptLibrary(root)` modeled on `ensureScriptSrc` (`assets.cppm:1057`): `create_directories`
  `root/"library"`, write `library/se.lua` from a new `inline constexpr std::string_view SeLuaDefs`
  (sibling of `StarterScript` at `:1029`) and `.luarc.json` from a literal — both **only-when-absent** like
  the `example.lua` guard (`:1066`). Called at the same two sites `ensureScriptSrc` is (`loadProject`
  `:1179`, `createProject` `:1247`), so existing projects gain the files on open.
- `StarterScript` (`:1029`) and the `createProjectScript` template (`:1103`) switch to **colon methods** +
  `---@class Example : se.ScriptSelf` so `self.entity:get_position()` autocompletes. This is **runtime-
  equivalent**: `callInstanceMethod` always pushes `self` (`script_runtime.cpp:229`), so `function
  Example:on_update(dt)` binds the identical field as `function Example.on_update(self, dt)` — a pure
  authoring-style change. No-legacy: switch in place, do not keep dot-form examples beside it. The starter
  body also moves to `se.Vec3` orbit math (Phase 2) in the same cutover.

---

## 7. Backend changes (minimal, NO-LEGACY, no-Physics-import honored)

The only genuinely **new engine C++** (everything else is pure bindings or thin host bridges):

1. **Physics impulse/velocity — its own sub-task (the one non-pure-binding physics gap).**
   - `physics.cpp` / `physics.cppm` (`Saffron.Physics`): add Jolt-free-signature exports
     `applyImpulse(PhysicsWorld&, u64 entityUuid, glm::vec3)`, `addForce(…)`,
     `setBodyLinearVelocity(…)`, `bodyLinearVelocity(PhysicsWorld&, u64) -> glm::vec3`, mapping
     uuid→`JPH::BodyID` through the **existing** entity↔body map (`indexByBodyId` / `bodies[].uuid` /
     `bodyUuid()`, the same map raycast uses) and calling `BodyInterface::AddImpulse` / `AddForce` /
     `SetLinearVelocity` / `GetLinearVelocity`, **activating the body** and **guarding** non-Dynamic bodies
     (impulse on Static/Kinematic is a logged no-op). Must respect the fixed-step seam: applied between
     steps (the `simTick` order is physics→scripts, so an `on_update` call is between steps — fine; **not**
     callable mid-solve from a contact handler).
   - Host bridge: `ScriptHost::applyImpulse` / `setVelocity` / `getVelocity` `std::function`s on the
     `ScriptHost` struct (`script.cppm:97`), wired in `host.cppm:1199` closing over `state->physics`,
     taking/returning glm/POD only.
   - Control command: `apply-impulse` / `set-velocity` in `Saffron.Control` (keep-current drivable-state
     rule) + an `e2e` case.
2. **Host bridges over existing physics C++** (no new Physics functions): `ScriptHost::sphereCast` (over
   `sphereCastWorld`), `ScriptHost::moveCharacter` (writes `CharacterControllerComponent`),
   `ScriptHost::setRagdollBlend` / `enableRagdoll` / `ragdollState` (over the existing functions). Each a
   `std::function` field on `ScriptHost` + a lambda in `host.cppm:1199`. `Saffron.Script` gains **no
   import**; all args/returns are PODs (`ScriptRayHit` reused for spherecast).
3. **Input plumbing** (host, no new Window C++ in v1): widen `ScriptInputParams` (the `script-input` DTO) to
   carry mouse position/buttons/scroll; widen `ScriptHost` input state to two derived edge sets + a mouse
   snapshot; host diffs held sets per tick. Editor forwards mouse over the control plane. (Gamepad and
   engine-window SDL capture are deferred new-C++.)
4. **`luaToJson` helper** (`script_runtime.cpp`) — the inverse of `jsonToLua`; pure runtime code, no engine
   dependency.
5. **Scheduler + message-queue state on `ScriptHost`** — a pending-coroutine list + a message queue, pumped/
   flushed in `tickScripts`. Runtime host-struct state + tick-loop code, **no new module / no new import**.

No backend change adds an `import Saffron.Physics`/`Animation` to `Saffron.Script`; the module DAG is
preserved.

---

## 8. Phasing (dependency-ordered, independently shippable)

Each phase ends with its `docs/content/explanations/scripting/` page (+ `_index.md` row), its
`tests/e2e/script.test.ts` cases (clean-log assertion stays green), a scaffold/`se.lua` touch, and a
control command where it adds drivable state. Foundation first (registry write + transforms + Vec3 +
tooling); physics/character/events build on top. Detail lives in the per-phase files.

| # | File | Theme | New C++? | Depends on |
|---|---|---|---|---|
| 0 | `phase-0-research.md` | SOTA notes + the binding-table decision record | — | — |
| 1 | `phase-1-component-write.md` | generic `set/add/remove/has_component` (name-keyed gate in the binding) + transform completeness (`get_rotation/get_scale`, world getters, `look_at`) + `luaToJson` | **pure binding** | — |
| 2 | `phase-2-vec3-userdata.md` | `se.Vec3` userdata (read-write props, dual `__mul`, math), registered in **both** `newScriptVm` and `startScripts`, cut through every vector API + `inferField`/`injectFields` + scaffold + e2e | **pure binding** | 1 (uses world getters) |
| 3 | `phase-3-entity-lifecycle.md` | `spawn`/`destroy`/`set_parent`/`parent`/`children`/`find_all_by_name`/`find_by_uuid`, **deferred-flush** + `hierarchyDirty` relink; follow-camera recipe | **pure binding** | 1, 2 |
| 4 | `phase-4-input.md` | edge detection (`just_pressed/released`) + mouse over the `script-input` channel; **gamepad deferred** | **NEW C++** (host/DTO plumbing) | 2 (Vec3 for mouse) |
| 5 | `phase-5-physics-animation-bridges.md` | `spherecast`, **required** `move_character`, ragdoll bridges; animation via `set_component`; **impulse/velocity NEW Physics C++** (§7.1 sub-task) | mixed: bridges + NEW Physics C++ | 2 (Vec3 args), 1 (set_component) |
| 6 | `phase-6-events-timers.md` | `entity:send`/`se.broadcast` bus + `se.wait`/scheduler (coroutine lib), resumed **inside** the tick window | host-struct state (no new module) | 3 (flush discipline) |
| 7 | `phase-7-luals-defs-and-codegen.md` | **7a** hand-written `library/se.lua` + `.luarc.json` scaffold + colon-method/`---@class` switch + **gating tripwire** in `check.sh`; **7b** declarative binding table + generator, delete hand-written `se.lua` | tooling | 1–6 (surface stable) |

**Note on 7a sequencing:** the `se.lua` + `.luarc.json` + tripwire can land **early** (right after Phase 1)
as immediate DX, then each later phase appends its annotations and the tripwire keeps them honest; 7b's
generator cutover is the last step once the surface stops moving. The follow-camera **recipe** doc/example
ships with Phase 3 (needs world getters + `look_at` from 1/2 and `set_parent`/spawn from 3).

---

## 9. Open risks the verifier should check

1. **Vec3 cutover completeness.** `se.Vec3` must be bound in **both** `newScriptVm` (so
   `Class.properties = { x = se.vec3(...) }` works in the `readScriptSchema` throwaway VM) **and**
   `startScripts`. The property form must be `addPropertyReadWrite` (the single-arg `addProperty` is
   read-only, `Namespace.h:886`). `__mul` must register **both** operand orders (vec\*scalar and
   scalar\*vec) — verify the metamethod overload set compiles on a value class (compile-probe that one
   combination). `inferField` must **detect** the userdata default **and switch extraction** to read its
   `x`/`y`/`z` (not `lua_rawgeti`) while still emitting the 3-number JSON array — the concrete failure mode
   for a wrong extraction is a silently-zeroed default. `injectFields`' `pushTableCopy` table-branch
   (`:378`) must be rewired so a userdata default is value-pushed per instance, not shared. Every `{x,y,z}`
   site (`getPosition`, `pushVec3Table` contact manifolds, raycast `point`/`normal`, all 13 e2e cases
   including the **setter** `set_position(x,y,z)` call sites, the scaffold orbit math + its `Math.hypot`
   assertion at `script.test.ts:416`, docs) must flip in one change — a missed site leaves two vector
   shapes (no-legacy violation).
2. **`set_component` structural-component gate.** Confirm the gate refuses the registered **name strings**
   `Relationship`/`SkinnedMesh`/`Bone`/`FootIk`/`BonePhysics`/`Collider`/`Rigidbody`/`KinematicBones`
   (`scene_edit_components.cpp:102`–`138`), lives **in the binding** (not in `ComponentTraits`, which is
   shared with scene-load), and that `set_parent` is the **only** hierarchy mutation path. `deserialize`
   auto-adds absent components (`scene.cppm:1248`), so an ungated write could desync the live Jolt world /
   hierarchy caches mid-play.
3. **`move_character` is required, not sugar.** Confirm `set_component("CharacterController", …)` cannot
   write `desiredVelocity`/`verticalVelocity` (serde resets/omits them,
   `scene_component_serde.generated.cpp:669`–`687`), so the named bridge is the only path and is **not** a
   redundant second write path. The `jump?` arg maps to the fixed `verticalVelocity = 5.0f`.
4. **Impulse/velocity determinism.** New Physics writes must respect the fixed-step accumulator and be
   applied between steps (raycast is read-only and safe; force/velocity are not). Verify they are **not**
   callable mid-solve from a contact handler, and that non-Dynamic bodies are a logged no-op.
5. **Deferred-flush correctness.** `tickScripts` iterates the **live** `host.instances` by reference
   (`:583`) — there is no snapshot, so spawn/destroy/set_parent must be queued and flushed after the
   instance loop with **one** `relinkHierarchy` (not per-op); self-destroy deferred so the handle survives
   the current handler; spawned entities' `ScriptComponent` not instantiated until next play (documented).
6. **Coroutine resume window.** The scheduler must resume coroutines **inside** the tick (with
   `currentScene` bound), or handle ops in a resumed coroutine become silent no-ops. Verify a faulting
   coroutine/handler is contained (pcall + traceback → script-error ring), never a VM crash.
7. **Input source-of-truth singularity.** Edges + mouse come over the **one** `script-input` control-plane
   channel (not a second SDL feed); headless e2e must still drive them. Confirm no `se.gamepad_*` is bound
   or annotated (no engine path).
8. **No duplicate name.** `se.get_entity_by_name` stays the single first-match name (no second
   `se.find_by_name`); only `se.find_all_by_name`/`se.find_by_uuid` are added as distinct-purpose bindings.
9. **`se.lua` tripwire is gating and covers both TUs.** It must run as a **gating** step in `check.sh`
   (not optional) and enumerate `se.log`/`se.Vec3` (bound in `newScriptVm`, `script.cppm:238`) alongside
   the `startScripts` bindings — or it false-positives. A 7b generator must read the declarative table,
   **never** re-parse the imperative `.addFunction` C++.
10. **No module-DAG break.** `Saffron.Script` still imports only `Core` + `Scene`; every physics/animation
    reach is a `ScriptHost` `std::function` over a POD. Any `import Saffron.Physics` is a hard failure.
11. **Scaffold idempotence.** `library/se.lua` + `.luarc.json` written only-when-absent at both
    `createProject` and `loadProject` sites; a user-authored `.luarc.json` is never clobbered.
