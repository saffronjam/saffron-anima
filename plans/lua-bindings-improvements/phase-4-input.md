# Phase 4 — input: key-edge + mouse (gamepad deferred)

**Status:** COMPLETED

The biggest **needs-new-C++** phase. Today a script sees only held keys, and not from SDL directly — the
host holds `ScriptHost::inputKeys` (`script.cppm:104`), a `std::unordered_set<std::string>` forwarded from
the editor by the `script-input` control command (`control_commands_scene.cpp:1700`). There is no mouse, no
edge detection, no gamepad anywhere the script can reach, and the `Window` struct (`window.cppm:29`–`33`)
emits **only** `onClose`/`onResize`/`onKeyPressed`/`onKeyReleased`/`onFileDropped` — **no mouse signal at
all.** So this phase plumbs input through the control plane + host *before* it can be bound.

## Current path (verified)

- `window.cppm:29`–`33`: `onClose`, `onResize`, `onKeyPressed`, `onKeyReleased`, `onFileDropped`. **No mouse,
  no scroll, no gamepad signal.** (An untyped `eventSinks` raw-SDL forward exists at `:35`–`37` for the
  gizmo/fly-camera, but no typed mouse signal.)
- The host passes the held-key set into `startScripts`; `is_key_pressed(key)` does a `contains`
  (`script_runtime.cpp:447`).
- The control command `script-input { keys }` (`control_commands_scene.cpp:1700`) is how the **editor**
  forwards which keys the focused viewport sees — this is a headless host, so input arrives over the control
  plane, **not** from the host's own SDL window during editor-driven play.

## One input source of truth (LOCKED): the `script-input` control-plane channel

Edges and mouse are derived/forwarded over the **one** `script-input` channel — **not** a second SDL-capture
feed. This keeps headless e2e drivable (the test seam stays the `script-input` command) and avoids two input
sources. Engine-window SDL capture is explicitly **not** pursued in v1.

### 1. Widen the host input state + DTO

Replace the bare `inputKeys` set with a `ScriptInputState` POD the host owns:

- `held: set<string>` — keys down this tick (exists; the editor keeps sending this).
- `pressed: set<string>` / `released: set<string>` — **edge sets derived host-side** by diffing the incoming
  `held` against the **previous tick's** `held` (stored on `ScriptHost`). The editor keeps sending only
  `held`; no edge fields on the wire. A key that flips down-and-up *between* two snapshots is missed —
  acceptable for an editor-driven play loop (the editor sends at input frequency); document the limitation.
  `just_pressed(key)` is true for exactly the tick after the key first appears in `held`.
- `mouse: { x, y, dx, dy }` (viewport-relative position + per-tick delta), `mouseButtons: set<string>`
  (`left`/`right`/`middle`), `scroll: number`. These need **new `script-input` DTO fields** in
  `control_dto.cppm` (today only `keys`), forwarded by the editor (which already tracks viewport pointer
  position for the gizmo). `dx`/`dy` are derived host-side from successive `mouse` snapshots (same diff
  pattern as edges).

### 2. Bind it (`script_runtime.cpp`, the `se` namespace)

| Lua API | Reads | Tag |
|---|---|---|
| `sa.is_key_pressed(key)` (held) | `inputState.held` | exists; keep |
| `sa.just_pressed(key)` / `sa.just_released(key)` | `inputState.pressed` / `.released` | edges |
| `sa.mouse_position() -> sa.Vec3` (z=0) | `inputState.mouse.{x,y}` | uses Phase 2 `Vec3` |
| `sa.mouse_delta() -> sa.Vec3` (z=0) | `inputState.mouse.{dx,dy}` | |
| `sa.mouse_button(n) -> bool` | `inputState.mouseButtons.contains(n)` | `n` ∈ `left`/`right`/`middle` |
| `sa.mouse_scroll() -> number` | `inputState.scroll` | |

All the same `contains`/field-read shape as today's `is_key_pressed`. Returning `sa.Vec3` is why this phase
depends on Phase 2.

## Gamepad — deferred (LOCKED, not annotated)

There is **no gamepad anywhere in the tree** (no `SDL_Gamepad` use, no gamepad signal in `window.cppm`, no
control-plane gamepad field). Exposing it is a full new path: SDL gamepad init + event handling, a
control-plane gamepad-state field, the host snapshot + binding. **Deferred to a follow-up;** do **not**
bind or annotate any `sa.gamepad_*` in `sa.lua` until the path exists (a binding with no backing C++ is
forbidden).

## Control command (the one genuinely-new wire surface)

`script-input` gains mouse fields (position/buttons/scroll) — a real new DTO surface, so it goes through
`control_dto.cppm` → `bun run tools/gen-control-dto/gen.ts` regeneration and the contract test. This is the
one phase that changes the command surface (the others reuse existing commands). Edges stay **derived
host-side**, so they add **no** wire fields. Optionally add a `get-script-input` inspector command for
debugging.

## Tests (`tests/e2e/script.test.ts`)

- Send a key via `script-input`, then clear it; a script counter that only increments on `just_pressed`
  increments exactly once, then stays put while held → `just_pressed` true for one tick then false.
- Send mouse position/buttons via the extended command; a script reads `sa.mouse_position()` /
  `sa.mouse_button("left")` and writes a derived transform; `inspect` confirms.

## Docs

New `docs/content/explanations/scripting/script-input.md` (or a section in `script-components-and-runtime.md`)
covering held vs edge keys, mouse, the editor-forwards-input model (one control-plane channel), the
between-snapshots edge-miss limitation, and the gamepad gap. Update `_index.md`.

## Constraints honored

NO-LEGACY (one input channel; edges derived, not a second feed; no duplicate held-key path), Saffron.Script
imports only Core+Scene (input state is a host POD, no Window import into Script), sandbox unchanged. The
DTO change is a real surface change — regenerate the protocol, never hand-edit generated files.

## Verification gate

`make engine`, `make prepare-for-commit`, `make e2e` green, **`bun run check`** (the protocol regen for the
extended `script-input` DTO) clean, and the **contract test passes** (live `help` matches the regenerated
manifest — this phase changes the command surface, unlike Phases 1–3).
