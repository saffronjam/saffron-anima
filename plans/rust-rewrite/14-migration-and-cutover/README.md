# 14 — Migration and cutover: the binary-boundary flip and parity sign-off

This area is the end of the rewrite, not a subsystem: it owns the *mechanism* by which the running
editor stops spawning the C++ `SaffronAnima` and starts spawning the Rust `saffron-host`, and the
*sign-off* that proves the flip is safe. Nothing here ports engine code — every engine phase lives in
areas `00`–`13`. This area exists because the cutover is a deliberate, gated act with its own
acceptance bar, and because deleting `engine-old/` (NO LEGACY) is the last phase of the whole plan.

The strategy is the feasibility study's option (C), already locked: a **greenfield parallel binary**
that speaks the *identical* frozen contracts (the JSON-over-unix-socket control envelope and the
POSIX-shm BGRA8 frame ring), so the editor — frontend **and** the already-Rust `editor/src-tauri/` —
runs unchanged on either engine. The boundary that lets either engine run is a single env var,
`SAFFRON_ANIMA_BIN`, read by both the editor spawn site and the e2e harness. The whole cutover is:
keep the editor on C++ until the Rust binary passes the full gate, then point that env var at the Rust
binary and delete the C++ tree.

## 1. Why a binary boundary, not an in-process bridge (NO LEGACY)

The feasibility study rejected an in-process C-ABI bridge (option B): the engine's seams are deep
by-reference aggregates (`Renderer&`, `EngineContext` of six live references), not C-ABI-friendly
boundaries, so a bridge would FFI-wrap nearly the whole surface and violate the no-compat-shims rule.
The only clean C-ABI boundaries that already exist are cross-*process* — the shm ring and the control
socket — and those are exactly what the editor consumes. So the natural "bridge" is the existing wire
contract, and "running either engine" is implemented at the **binary** boundary: the editor spawns one
child process; which binary that is, is a one-line env override.

This is grounded in real code that needs **zero changes** to support the flip:

- `editor/src-tauri/src/lib.rs:186` — `engine_binary()` reads `SAFFRON_ANIMA_BIN`, defaulting to
  `build/debug/bin/SaffronAnima` (the C++ binary). The editor spawns whatever this resolves to and
  glues to it only through the shm name (`viewport_shm_name`, `lib.rs:181`) and the control socket.
- `tests/e2e/harness.ts:18` — the e2e harness reads the *same* `SAFFRON_ANIMA_BIN`, defaulting to the
  same C++ path. So the same suite drives either engine by setting one env var.

Because both consumers already key off `SAFFRON_ANIMA_BIN`, the cutover changes **no editor source and
no test source** — it changes a default (and a justfile/CI env), and that is the entire point of the
frozen-contract strategy.

## 2. The two-phase walk to the flip

The cutover is not a single moment; it is a *qualification* (the Rust binary earns the flip by passing
the full gate against the parity rig) followed by the *flip* (re-point the default) followed by the
*deletion* (remove `engine-old/` and the parity rig). Three phases:

| Phase | What | Depends on |
|---|---|---|
| `phase-1-parity-signoff` | run the full gate + the cross-engine parity rig (golden images, Jolt traces, serde byte-equality) with `SAFFRON_ANIMA_BIN` pointed at the Rust `saffron-host`, against the C++ binary as oracle; produce the go/no-go sign-off | `13-testing-and-verification:phase-7-cross-engine-parity-rig`, `13-testing-and-verification:phase-9-reproducible-gate-orchestration`, all engine areas (`08`,`09`,`11`,`12` complete) |
| `phase-2-binary-flip` | flip the `SAFFRON_ANIMA_BIN` default to the Rust binary in the editor spawn + the e2e harness + the justfile/gate env; the editor runs unchanged on the Rust engine | `phase-1-parity-signoff` |
| `phase-3-retire-cpp-tree` | delete `engine-old/`, the C++ build references repointed in `01-build-and-toolchain:phase-1`, the parity rig, and the C++-only tooling (`tools/gen-control-dto`, `tools/sa`, `tools/check-script-defs`, `cmd/sa`); the tree is Rust-only | `phase-2-binary-flip` |

The flip (phase 2) is intentionally trivial — a default change — because all the *risk* is front-loaded
into phase 1's sign-off. The deletion (phase 3) is last and irreversible, executed only once the Rust
binary has been the editor's default long enough to be trusted (the plan does not gate phase 3 on a
calendar; it gates it on phase 2 being green and the author choosing to delete — NO LEGACY says delete
after `COMPLETED`, and that is the trigger).

## 3. The go/no-go sign-off bar (phase 1)

The Rust binary qualifies for the flip only when **all** of the following are green with
`SAFFRON_ANIMA_BIN` pointed at `target/<profile>/saffron-host` (or the toolbox-built path):

1. **The reproducible gate** (`13:phase-9` / `01:phase-6`) passes end-to-end: workspace builds, shaders
   compile, present-only smoke is validation-clean, the control-schema contract test passes, the
   project/asset smoke passes, the frontend builds, `cargo test --workspace` is green, clippy/fmt clean.
2. **The full `tests/e2e` bun suite** (the 81 `*.test.ts`) passes against the Rust binary — the same
   assertions that pass against the C++ binary, run by flipping `SAFFRON_ANIMA_BIN` (the harness is
   engine-language-agnostic by design).
3. **The cross-engine parity rig** (`13:phase-7`) is clean on its three diffs: golden render images
   match within tolerance, the Jolt determinism scenario's sim trace is bit-identical C++-vs-Rust, and
   a scene/material/model authored by one engine loads byte-identically in the other.
4. **The three subsystem go/no-go gates** that the feasibility study front-loaded are already green
   (they gate their own areas long before cutover, and the sign-off re-confirms them): physics
   cross-arch determinism (`05:phase-5`), the ECS speed gate (`03:phase-2`), and the renderer/shm
   bring-up (`08:phase-3` + a validation-clean offscreen frame shown in the unchanged editor).

If any item is red, the flip does not happen; the failing area's phase is reopened. The sign-off is a
checklist run, not a new test harness — every check it runs is owned by an earlier phase.

## 4. What this area explicitly does NOT do

- It does **not** introduce a feature flag, a runtime engine selector, or a "use the old path for X"
  switch. There is one editor child process; it is one binary or the other, chosen at spawn by the env
  default. No dual-engine runtime.
- It does **not** keep `engine-old/` alive past phase 3 "just in case" — the parity rig and the C++
  tree are deleted together (NO LEGACY). Project-data migration is out of scope (start a fresh project),
  so there is no save-format bridge to retire.
- It does **not** own the parity rig's *construction* — that is `13-testing-and-verification:phase-7`.
  This area *operates* it for the sign-off and *deletes* it at retirement.

## 5. Grounding (real files / symbols)

| What | File | Symbols |
|---|---|---|
| The binary-boundary override (editor spawn) | `editor/src-tauri/src/lib.rs` | `engine_binary()` (`:186`, reads `SAFFRON_ANIMA_BIN`), `viewport_shm_name` (`:181`), the child spawn |
| The same override (test harness) | `tests/e2e/harness.ts` | `SAFFRON_ANIMA_BIN` (`:18`), `Engine.boot` |
| The frozen shm reader (unchanged across the flip) | `editor/src-tauri/src/wayland_viewport.rs` | `SHM_MAGIC`, `step_view`, `open_shm` |
| The migration strategy verdict (option C) | `plans/rust-rewrite-feasibility.md` | §6 "(C) Hybrid", §8 go/no-go |
| The C++ build references repointed (then deleted) | `01-build-and-toolchain/phase-1-relocation-repoint.md` | `add_subdirectory(engine-old)`, the `tools/*` path repoints |
| The cross-engine parity rig (operated here, built in 13) | `13-testing-and-verification/phase-7-cross-engine-parity-rig.md` | golden images, sim traces, serde byte-equality |
| The reproducible gate orchestrator (the sign-off's pass bar) | `13-testing-and-verification/phase-9-reproducible-gate-orchestration.md` | the gate sequence |

## 6. Phases in this area

1. `phase-1-parity-signoff` — qualify the Rust binary against the full gate + parity rig; produce the
   go/no-go sign-off.
2. `phase-2-binary-flip` — flip the `SAFFRON_ANIMA_BIN` default to the Rust binary; editor + e2e + gate
   run unchanged on the Rust engine.
3. `phase-3-retire-cpp-tree` — delete `engine-old/`, the C++ tooling, and the parity rig; the tree is
   Rust-only.
