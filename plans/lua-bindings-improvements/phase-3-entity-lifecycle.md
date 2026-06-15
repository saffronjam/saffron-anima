# Phase 3 — entity lifecycle, lookup, and hierarchy

**Status:** COMPLETED

Scripts can move existing entities but cannot create, destroy, or re-parent them, and `find` is a single
first-match-by-name. `Saffron.Scene` already has every backing function, so this is **all pure bindings** —
with one sharp constraint around spawning into the throwaway play duplicate and one around mutating the live
instance loop.

## New `sa` globals + `sa.Entity` methods (LOCKED)

| Surface | Signature | Backing (`scene.cppm`) | Notes |
|---|---|---|---|
| `sa.spawn(name)` | `-> sa.Entity` | `createEntity` (`:618`) | mints a fresh uuid; entity lives in the play duplicate only |
| `entity:destroy()` | `-> nil` | `destroyEntity` (`:630`) | **deferred to flush** (see below); handle then degrades to a logged no-op |
| `entity:set_parent(other)` | `-> bool ok` | `setParent` (`:1009`, relinks `:1069`, guards self/cycle/dangling) | returns its `Result` as ok / err-logged; **the only hierarchy mutation path** |
| `entity:parent()` | `-> sa.Entity \| nil` | `RelationshipComponent.parent` + `findEntityByUuid` (`:735`) | nil at root |
| `entity:children()` | `-> { sa.Entity }` | `RelationshipComponent.children` cache (relinkHierarchy-fresh) | |
| `sa.find_all_by_name(name)` | `-> { sa.Entity }` | `forEach<NameComponent>` (the existing `:451` walk, multi-match) | the multi-match the existing `get_entity_by_name` can't give |
| `sa.find_by_uuid(uuid)` | `-> sa.Entity \| nil` | `findEntityByUuid` (`:735`) | uuid lookup |
| `entity:uuid()` | `-> string` | `IdComponent.id.value` (decimal string, matching the wire) | stable cross-tick identity for cross-entity refs |

**Name discipline (no-legacy, LOCKED).** The existing first-match `sa.get_entity_by_name`
(`script_runtime.cpp:451`) **stays as-is** — the plan does **not** add a duplicate `sa.find_by_name` for the
same `NameComponent` lookup. The new surface is the **multi-match** `sa.find_all_by_name` (a distinct purpose
the existing binding cannot serve) and the **uuid** lookup. Three distinct names, no overlap.

**Tags are deferred (LOCKED).** There is no `TagComponent` today (only `NameComponent`), so find-by-tag would
need new C++ (a registered component + serde + a control command). Out of scope per the README v1/deferred
matrix; ship `find_all_by_name` now. Do **not** annotate or bind any `find_by_tag` in `sa.lua`.

## The spawn-into-play-duplicate constraint (critical)

Scripts run against the **throwaway play duplicate**; `stop` discards it (that discard is the authored-scene
restore — see the e2e `move.lua` test). So:

- `sa.spawn` adds to the duplicate; the entity **vanishes on stop** by design. Correct (the authored scene
  is never mutated by play), but document loudly — authors expect spawns to be transient, like Roblox
  `Instance.new` in a running game.
- A spawned entity is bare (`Name` + `Transform` + a default `RelationshipComponent`). Its `ScriptComponent`,
  **even if added via Phase 1's `add_component("Script")` + `set_component`, does NOT instantiate this play
  session** — `startScripts` already ran. It would attach on the *next* play. Document this clearly.
- Spawning a *configured* prefab (mesh + material + components in one call) has no analogue — Saffron has no
  prefab/template asset. A script spawns bare and adds components via Phase 1. Note prefab spawning as a
  future direction, out of scope.

## Reentrancy — deferred-flush (LOCKED)

`tickScripts` iterates the **live** `host.instances` vector **by reference**
(`script_runtime.cpp:583`; same direct-reference loop in `startScripts:562`, `dispatchContact:628`,
`stopScripts:662`) — **there is no pre-existing snapshot.** So mutating the container or `entt` storage
mid-loop is unsafe. Spawn / destroy / set_parent during a tick are therefore **queued and flushed after the
instance loop** — the deferred flush, not a snapshot, is the safety mechanism:

- Add a small per-host structural-op queue on `ScriptHost` (`{ kind: spawn|destroy|reparent, args… }`) plus a
  `bool host.hierarchyDirty`.
- `sa.spawn` / `entity:destroy` / `entity:set_parent` **enqueue** an op and set `hierarchyDirty`; they do
  **not** touch `entt` storage during the loop. `sa.spawn` returns the freshly-minted handle immediately (it
  can create the entity right away — `createEntity` only touches the new entity's storage, no view iteration
  — but defer the **relink** so caches settle once).
- After the instance loop in `tickScripts` (and `startScripts`/`dispatchContact`, which also iterate
  instances), drain the queue, then if `hierarchyDirty`, run **one** `relinkHierarchy` (`scene.cppm`) and
  clear the flag. One relink per tick when any structural op happened (`createEntity` does **not** relink,
  `setParent` does — the single flush normalizes this), mirroring how the editor batches structural edits.
- **Self-destroy is deferred to flush** so the handle stays valid for the rest of the current handler; after
  the flush `:valid()` flips false (entt `valid` returns false for a destroyed handle), and any subsequent
  `set_position` etc. degrades to the existing `transformScene`/`logWarn` no-op.

## Follow-camera recipe (ships here, no new C++)

The follow camera is a **documented Lua recipe**, not an engine API (README §4 Camera). It needs only Phase
2's world getters + `sa.look_at` + `sa.lerp` and this phase's `set_parent`/spawn. Ship a `follow_camera.lua`
recipe in the camera/scripting docs page (and optionally a scaffold example): a separate camera entity whose
script lerps its world position toward `target:get_world_position() + offset`, orients with `sa.look_at`, and
uses `sa.raycast` for occlusion pull-in.

## Control command (drivable-state rule)

`sa.spawn` / `destroy` / `set_parent` map to existing control commands (`add-entity`, `destroy-entity`,
`set-parent`) used in the e2e harness — **no new command needed**; the capability is exposed to Lua, not
added to the engine.

## Tests (`tests/e2e/script.test.ts`)

- A spawner script `sa.spawn("Bullet")`, adds a Transform, sets a position; during play the entity is present
  (count / lookup by name), and **after `stop` it is gone** (entity count returns to baseline — the discard).
- `set_parent`: spawn A, spawn B, `b:set_parent(a)`; after the tick flush, `b:parent():uuid() == a:uuid()`
  and `a:children()` contains B. A self-parent / cycle attempt returns `false` and leaves play `playing`.
- `entity:destroy()`: after the flush `entity:valid()` is false; a subsequent `set_position` in a later tick
  is a logged no-op (play stays `playing`).
- `find_all_by_name` with two same-named entities returns both; `find_by_uuid` round-trips a spawned entity's
  `uuid()`.

## Docs

Add an entity-lifecycle section to `script-components-and-runtime.md` (spawn is play-duplicate-scoped and its
`ScriptComponent` won't run until next play; hierarchy ops relink once at tick end; `set_parent` is the only
reparent path). Add the follow-camera recipe to the camera/scripting page. Update `_index.md`.

## Constraints honored

NO-LEGACY (one reparent path, one first-match name; new bindings have distinct purposes), Saffron.Script
imports only Core+Scene (lifecycle is all Scene), sandbox unchanged, dead-handle ops are logged no-ops. No
generated file hand-edited.

## Verification gate

`make engine`, `make prepare-for-commit`, `make e2e` green; the spawn/destroy/relink path leaves the
validation log clean and `stop` restores the entity count.
