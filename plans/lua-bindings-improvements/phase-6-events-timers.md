# Phase 6 — messaging + a coroutine scheduler

**Status:** COMPLETED

The two missing scripting primitives that turn isolated `on_update` loops into a system: inter-script
**messaging** (Defold/Unity `SendMessage`-shaped) and a **timer/scheduler** (Roblox's `task.wait/spawn/
delay`). Both are **pure Lua over the already-enabled `coroutine` lib + the single shared VM** (all instances
share one `lua_State`, `script.cppm:93`/`:99`). **No new module and no cross-module import** — but they do
add new **`ScriptHost` host-struct state** (a message queue + a coroutine scheduler) and new **`tickScripts`
flush/pump code**. Depends on Phase 3 (the deferred-flush discipline).

## Messaging — one entity-targeted bus (LOCKED)

Pick **exactly one** mechanism (no UnityEvent-style registry beside it — that would be the duplicate the
no-legacy rule forbids):

- `entity:send(handler_name, ...)` — queue `{ target_uuid, handler_name, args }` during the tick.
- `sa.broadcast(handler_name, ...)` — the global variant over every instance.
- **Flush after the instance loop** (the same reentrancy discipline as spawn/destroy, Phase 3 —
  `tickScripts` iterates the live `host.instances` by reference at `:583`, so dispatch cannot run
  mid-loop): drain the message queue, and for each message invoke each matching instance's
  `self:<handler_name>(sender, ...)` via the existing `callInstanceMethod` machinery (`script_runtime.cpp:229`,
  which always pushes `self`).
- Each dispatched call runs under the existing `pcall` + traceback (pause-on-error containment); one bad
  handler does not abort the rest or crash the VM, and the error lands in the script-error ring.

This gives entity-to-entity and system-to-script messaging (e.g. a `player_died` broadcast many scripts
react to) with no C++ event bus. **Cross-tick lifetime:** the queue is host-owned, drained each tick, and
cleared in `stopScripts` alongside instances/classes.

### Property-changed signals — NOT in v1 (LOCKED)

Roblox's `GetPropertyChangedSignal(prop)` fires on component-field changes. Replicating it needs the engine
notifying Lua on every component write — expensive and not plumbed (the registry has no change-notification
hook). **Do NOT replicate it in v1.** A script that wants to react to a value polls it in `on_update` (cheap,
deterministic) or another script `send`/`broadcast`s an explicit message on change. Note it as a future
direction gated on a registry change-hook.

## Timers / scheduler — over the enabled `coroutine` lib (LOCKED)

The `coroutine` lib is already open (`script.cppm:237`, `LUA_COLIBK`). Build a Roblox-`task`-style scheduler
entirely in the runtime, injected as a Lua prelude at VM creation, driven by the existing tick:

- `sa.wait(seconds)` — yields the current coroutine; resumed by the scheduler once `seconds` of accumulated
  `dt` have passed. **Only valid inside a coroutine** (a `sa.spawn_task`'d function), never in a bare
  `on_update` (which can't yield across the C boundary cleanly) — a `wait` outside a coroutine is a logged
  error (matching Roblox), not a crash.
- `sa.spawn_task(fn, ...)` — wrap `fn` in a coroutine, resume it immediately (Roblox `task.spawn`).
- `sa.delay(seconds, fn)` — schedule `fn` after a delay (Roblox `task.delay`).

### Implementation — a host-owned ready/sleep queue

Add a scheduler to `ScriptHost`: a list of `{ thread (a Lua coroutine ref), wakeAtAccumulated }` + an
accumulated-time counter. Each `tickScripts`, **inside the tick window** (after the instance loop but while
`host.currentScene`/`currentRegistry` are still bound — a resume with `currentScene` null would make handle
ops silent no-ops):

1. advance the accumulated-time counter by `dt`,
2. resume every coroutine whose `wakeAt <= accumulated` (in order), under the traceback handler,
3. re-queue ones that yield again (another `wait`), drop finished ones; an errored one → the script-error
   ring (pause-on-error policy), contained, never a VM crash.

~60 lines in `script_runtime.cpp` + a few `ScriptHost` fields, no new module, no C++ outside `Saffron.Script`.
It composes with `on_update` (which still runs every tick); coroutines are an additional, optional
concurrency primitive. The scheduler is cleared in `stopScripts`.

**Sandbox note:** `sa.wait` is coroutine-yield-based, never `os.clock`/busy-wait (`os` is withheld anyway).
Timing comes from accumulated `dt`, deterministic with the play loop.

## Control command

Messaging + timers are in-VM script constructs; they add no engine state to drive from the CLI. **No new
control command.** (An optional debug `get-script-status` readout of the live coroutine count is a
nice-to-have, not required.)

## Tests (`tests/e2e/script.test.ts`)

- `sa.spawn_task` a coroutine that `sa.wait(0.2)` then sets a position; assert the position changes only
  after ~0.2s of accumulated dt, not immediately.
- Messaging: script A defines a handler that moves its entity; script B `entity:send`s A (or
  `sa.broadcast`s); after the flush, A's entity moved. A handler that `error`s is contained (play stays
  `playing`, the error lands in the ring) and the **other** handlers still run.
- `sa.wait` called from a bare `on_update` is a logged error, not a crash (play stays `playing`).

## Docs

New `docs/content/explanations/scripting/script-events-and-timers.md` (the message bus, the coroutine
scheduler resumed inside the tick window, the no-property-changed-signal decision), update `_index.md`.

## Constraints honored

NO-LEGACY (one messaging mechanism, one scheduler; no second event registry), **no new module / no
cross-module import** (it is new `ScriptHost` host-struct state + tick-loop code, all inside `Saffron.Script`),
sandbox holds (no `os`/`io`; the scheduler is coroutine-based). Errors are contained per the pause-on-error
policy.

## Verification gate

`make engine`, `make prepare-for-commit`, `make e2e` green; the scheduler is deterministic against the tick
(the wait test passes on the software-GPU CI cadence); the sandbox probe still holds (no `os`/`io`); a
faulting coroutine/handler is contained, never a VM crash.
