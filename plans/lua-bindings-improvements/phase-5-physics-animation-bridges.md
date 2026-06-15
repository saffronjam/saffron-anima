# Phase 5 — physics + character + ragdoll bridges (+ the one new Physics C++)

**Status:** COMPLETED

`Saffron.Script` must **not** import `Saffron.Physics` or `Saffron.Animation`. Every reach is a host-bound
`std::function` over a glm-light / Jolt-free POD — the `ScriptHost::raycast` template (`script.cppm:107`,
wired at `host.cppm:1199`, guarded on `state->physics.has_value()` `:1202`). Each item below is flagged
**bridge** (the C++ API exists, just wire a `std::function`) or **NEW C++** (a new `Saffron.Physics` function
first). Depends on Phase 2 (Vec3 args/returns) and Phase 1 (`set_component` for animation).

## The bridge pattern (the rule)

1. Add a `std::function<…>` field to `ScriptHost` (`script.cppm`, next to `raycast` at `:107`),
   taking/returning PODs only (no Jolt; glm only as far as the existing `ScriptRayHit` does) — mirror
   `raycast`'s shape.
2. Bind an `se`/`se.Entity` function that calls the closure (the `raycast` binding in `startScripts` is the
   exact shape — guard on the closure being set; an unset closure = a safe default / logged-miss no-op).
3. Wire it in `host.cppm` next to `state->script.raycast = [state](…){ … }` (`:1199`), capturing the live
   `PhysicsWorld` and translating to/from the POD. `host.cppm` already imports both `Physics` (`:41`) and
   `Script` (`:42`), so new bridges live legally there.

## Physics — what exists vs what is missing (verified in `physics.cppm`)

| Lua API | C++ backing | Tag |
|---|---|---|
| `se.spherecast(origin, dir, radius, maxDist) -> RayHit` | `sphereCastWorld` (`physics.cppm:139`) | **bridge** — `ScriptHost::sphereCast`, reuses `ScriptRayHit` POD verbatim (twin of raycast) |
| `entity:move_character(velocity, jump?)` | writes `CharacterControllerComponent.desiredVelocity` / `verticalVelocity` (the `move-character` command body, `control_commands_physics.cpp:224`/`:227`) | **REQUIRED bridge** (see below) |
| `entity:enable_ragdoll()` / `entity:disable_ragdoll()` | `enableRagdoll`/`disableRagdoll` (`physics.cppm:145`/`:148`) | **bridge** — rig uuid is the entity's id |
| `entity:set_ragdoll_blend(active?, bodyWeight?, …)` | `setRagdollBlend` (`physics.cppm:181`) | **bridge** |
| `entity:ragdoll_state() -> table` | `ragdollState` (`physics.cppm:193`, returns a POD `RagdollState` `:186`) | **bridge** |
| `entity:apply_impulse(Vec3)` / `entity:set_velocity(Vec3)` / `entity:add_force(Vec3)` / `entity:get_velocity() -> Vec3` | **none — no force/impulse/velocity API exists** | **NEW C++** (§ below) |

### `move_character` is a REQUIRED bridge, not sugar (LOCKED)

The generic `set_component("CharacterController", { desiredVelocity = … })` path is silently a **no-op** for
the velocity fields: `characterControllerComponentFromJson` resets `desiredVelocity`/`verticalVelocity`/
`onGround` to zero on every deserialize, and `…ToJson` deliberately omits them
(`scene_component_serde.generated.cpp:669`–`687`, comment "the runtime velocity/ground state serialize as
their defaults — move-character writes them at play time"). So `move_character` is the **only** path to those
fields, **not** a redundant second write path — no-legacy is satisfied. The `jump?` boolean maps to the
existing fixed `verticalVelocity = 5.0f` jump impulse (`control_commands_physics.cpp:227`), not an arbitrary
value, preserving the `move-character` command's behavior. Wire it as `ScriptHost::moveCharacter(uuid,
glm::vec3 velocity, bool jump)` writing the component on the live duplicate.

### The force/impulse/velocity gap (the one real engine hole, §7.1)

**There is no force/impulse/velocity API at the `Saffron.Physics` C++ level today** — `physics.cppm` exports
none, `BodyInterface` is internal-only, and the only `SetLinearVelocity` is on the `CharacterVirtual` inside
`stepPhysics` (`physics.cpp:929`). A Lua `apply_impulse` needs, in order:

1. **New `Saffron.Physics` exports** (Jolt-free signatures) that reach the Jolt body for an entity:
   - `applyImpulse(PhysicsWorld&, u64 entityUuid, glm::vec3)`,
   - `addForce(PhysicsWorld&, u64 entityUuid, glm::vec3)`,
   - `setBodyLinearVelocity(PhysicsWorld&, u64 entityUuid, glm::vec3)`,
   - `bodyLinearVelocity(const PhysicsWorld&, u64 entityUuid) -> glm::vec3`.

   These map uuid → `JPH::BodyID` through the **existing** entity↔body map (`indexByBodyId` /
   `bodies[].uuid` / `bodyUuid()`, the same map `raycastWorld` uses for hit mapping) and call Jolt's
   `BodyInterface::AddImpulse` / `AddForce` / `SetLinearVelocity` / `GetLinearVelocity`, **activating the
   body**. **Guard non-Dynamic bodies:** an impulse/force/velocity write on a Static/Kinematic body is a
   logged no-op. Must respect the fixed-step seam: the `simTick` order is physics→scripts (`host.cppm:1142`–
   `1189`), so an `on_update` write lands **between** steps (applied before the next `stepPhysics`) — fine.
   It is **not** callable mid-solve from a contact handler.
2. **A control command** (`apply-impulse` / `set-velocity`) in `Saffron.Control` so the capability is
   drivable/inspectable from the `se` CLI (the drivable-state rule) + an e2e case.
3. **Host bridges** `ScriptHost::applyImpulse` / `setVelocity` / `getVelocity` (`std::function` over
   glm/POD) + the `se.Entity` bindings.

This is the only sub-item touching three layers (Physics C++ → control command → bridge → binding); budget it
as the largest piece of Phase 5. Everything else physics-side is a pure bridge.

## Animation — `set_component`, no bridge (LOCKED)

Clip control is **field writes on the registered `AnimationPlayerComponent`** (`clip`/`time`/`speed`/
`playing`/`wrap`), and those fields **round-trip through serde** (`scene_component_serde.generated.cpp:299`–
`313`) — the same fields the `play-animation` control command writes (`control_commands_animation.cpp:264`).
So clip control rides Phase 1's generic `set_component` with **no animation import and no new bridge**:

```lua
entity:set_component("AnimationPlayer", { clip = clip_uuid, playing = true })
```

(the script supplies the clip uuid — there is no asset-lookup-by-name binding in v1). A thin
`entity:play_clip(uuid)` sugar is **deferred** — it would duplicate the `set_component` field write
(no-legacy). Contact callbacks (`on_contact`/`on_trigger_*`) already work via the contact ring →
`dispatchContact` — **kept unchanged.**

## Camera (no new C++)

The follow-camera recipe ships in **Phase 3** (it needs only world getters + `se.look_at` + `se.lerp` +
`set_parent`). Per-camera params (fov/near/far) ride `set_component("Camera", …)`. No physics/camera bridge
here.

## Tests (`tests/e2e/script.test.ts`)

- `se.spherecast` hits a body a thin ray misses (mirror the raycast test setup).
- `move_character`: a `CharacterController` entity; a script sets a desired velocity → over ticks it moves;
  `jump = true` raises it (the fixed impulse).
- `apply_impulse` (after the new C++ lands): a dynamic rigidbody gains velocity; `get_velocity` reads it
  back; an impulse on a static body is a logged no-op.
- `enable_ragdoll` / `set_ragdoll_blend`: drive a rig, assert `ragdoll_state().active`.
- animation: `set_component("AnimationPlayer", { clip = …, playing = true })`; `get_component(...).time`
  advances over ticks.

## Docs

New `docs/content/explanations/scripting/script-physics-and-animation.md` (the bridge pattern, the
physics/character/ragdoll API, the required `move_character` rationale, animation via `set_component`), update
`_index.md`. The new `applyImpulse`/`setVelocity` C++ also updates `docs/content/explanations/physics/` (its
own concept page) per the keep-docs rule.

## Constraints honored

NO-LEGACY (one path per op; `move_character` is the only velocity path, not a duplicate; no `play_clip`
beside `set_component`), **Saffron.Script imports only Core+Scene** (every physics/animation reach is a
`ScriptHost` `std::function` over a POD; the bridges live in `host.cppm`), sandbox unchanged, errors logged.
The new Physics C++ goes in `Saffron.Physics`, never imported by Script.

## Verification gate

`make engine`, `make prepare-for-commit`, `make e2e` green; **the new physics commands** make `bun run check`
+ the contract test mandatory; the deterministic physics step stays clean (validation log empty); the impulse
path respects the between-steps ordering and the non-Dynamic guard.
