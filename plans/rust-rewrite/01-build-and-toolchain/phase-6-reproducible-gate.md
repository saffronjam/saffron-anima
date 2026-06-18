# Phase 6 — The reproducible gate (`tools/ci/check.sh` rewrite)

**Status:** COMPLETED
**Depends on:** phase-3-xtask-shader-pipeline, phase-5-justfile-and-toolbox

## Goal

Rewrite the reproducible verification gate (`tools/ci/check.sh`) around Cargo + `xtask`, keeping the
toolbox/weston framing and every frozen-wire contract check, so `just check` runs the full gate inside
the toolbox under a headless display. After this phase there is one gate script that builds the
workspace, compiles shaders, runs the present-only smoke, the control-schema contract test, the project
smoke, the unit tests, the clippy/fmt lint, and the frontend build — the green-light a `just run` picks
up.

## Why this shape (NO LEGACY)

The C++ gate's structure is sound (build → smoke → contract → frontend, all inside the toolbox under
weston) and the frozen-wire checks (`check-control-schema`, `check-projects`, the present-only smoke
env) are unchanged because the wire and the shm ring are frozen. What changes is the *build half* (Cargo
+ `xtask` replace `cmake` + `gen.ts`) and two specific steps:

- The `gen.ts` + `git diff --exit-code` generated-file guard becomes `cargo run -p xtask gen` + a
  `git diff --exit-code` over the *Rust*-generated artifacts (`@saffron/protocol`, OpenRPC, manifest) —
  the same "generated output is checked in and must not drift" discipline, now driven by `xtask` (PP-7
  owns the codegen; this phase only orchestrates it in the gate).
- The `check-script-defs` drift step is **deleted**: the Luau type defs are generated from the binding
  source (PP-8), so the hand-written-overlay drift it guarded against cannot exist. It is replaced by an
  `xtask`-freshness check (generated `.luau` defs up-to-date), or removed entirely if PP-8 folds that
  into the gen-freshness diff. NO LEGACY — a tripwire for a hand-synced copy that no longer exists is
  dead and goes.

Two steps are *added* that the C++ gate could not have: `cargo test --workspace` (real `#[test]`s
replace the deleted in-engine self-tests, per the foundations no-self-test rule) and the clippy/fmt
lint folded into the gate (the Makefile ran lint separately; folding it in makes the gate the single
source of "is this green"). There is one gate; `make check` is retired with the rest of the `Makefile`
at cutover.

The present-only smoke, the control-schema contract test, and the project smoke keep their *exact*
invocation — same env vars (`SAFFRON_EXIT_AFTER_FRAMES`, `SAFFRON_CONTROL_SOCK`), same per-run socket
discipline — because they validate the frozen contract; the only change is the binary they point at
flips to the Rust host once it passes (the cutover phase owns the flip, not this phase).

## Grounding (real files/symbols)

- `tools/ci/check.sh` — the seven-step structure to rewrite:
  - the `bun run tools/gen-control-dto/gen.ts` + `git diff --exit-code` over the generated list →
    `cargo run -p xtask gen` + `git diff --exit-code` over the Rust-generated artifacts;
  - `cmake --preset debug && cmake --build build/debug -j1` → `cargo build --workspace` +
    `cargo run -p xtask shaders`;
  - the present-only smoke `SAFFRON_EXIT_AFTER_FRAMES=5 SAFFRON_CONTROL_SOCK=/tmp/sa-ci.sock
    "$REPO/build/debug/bin/SaffronAnima"` (lines 34–39) — same env, binary path flips at cutover;
  - `check-control-schema` (`bun run check.ts`) — unchanged;
  - `check-script-defs` (`bun "$REPO/tools/check-script-defs/check.ts"`) — deleted/replaced;
  - `check-projects` (`"$REPO/tools/check-projects/check.sh"`) — unchanged (it drives the live
    engine + `sa`, paths repointed in phase 1);
  - the frontend `cd "$REPO/editor" && bun run build && bun test` — unchanged;
  - the `set -uo pipefail` + `fail=1` accumulation + `ALL GATES PASSED`/`SOME GATES FAILED` framing —
    kept.
- `tools/ci/check.sh` (header, lines 5–12) — the toolbox + weston run framing
  (`toolbox run -c saffron-build`, `weston --backend=headless --socket=wl-ci --idle-time=0`,
  `WAYLAND_DISPLAY=wl-ci SDL_VIDEODRIVER=wayland`, `XDG_RUNTIME_DIR`) — kept verbatim.
- `Makefile` `check:` (line 117) — `"$(REPO)tools/ci/check.sh"`; `just check` invokes the rewritten
  script the same way.
- `13-testing-and-verification` — owns the *test strategy* (what each `#[test]`/e2e asserts); this phase
  owns the *gate orchestration* (running them in order in the toolbox). The two are referenced, not
  duplicated.

## Acceptance gate

- `tools/ci/check.sh` (rewritten) runs inside the toolbox under headless weston and executes, in order:
  codegen-freshness diff → `cargo build --workspace` + `xtask shaders` → `cargo test --workspace` →
  present-only smoke → `check-control-schema` → `check-projects` → frontend `bun run build` + `bun test`
  → `cargo clippy --workspace -- -D warnings` + `cargo fmt --check`.
- The script accumulates failures (`set -uo pipefail`, `fail=1` per step) and prints `ALL GATES PASSED`
  / `SOME GATES FAILED`, exiting with the right code — matching the C++ gate's contract.
- `just check` invokes the script; on the workspace built so far (foundations + build area) it passes:
  the workspace compiles, shaders build, the unit tests so far pass, clippy/fmt are clean.
- The `check-script-defs` step no longer exists in the script (replaced by the `xtask`-defs-freshness
  check or removed).
- The present-only smoke uses `SAFFRON_EXIT_AFTER_FRAMES` + a per-run `SAFFRON_CONTROL_SOCK` exactly as
  the C++ gate did (frozen contract); the binary path is the only thing the cutover phase later flips.
- `grep -nE 'cmake|gen\.ts|check-script-defs' tools/ci/check.sh` returns no hits (no CMake, no `gen.ts`,
  no script-def drift step survive in the Rust gate).
