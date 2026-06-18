# Phase 4 — A native-Rust e2e driver over the wire

**Status:** NOT STARTED

**Depends on:** 13-testing-and-verification:phase-3-bun-e2e-as-parity-harness, 11-sa-cli:phase-1-crate-and-socket-client

## Goal

Provide a thin native-Rust e2e harness so the engine team can write wire-driven tests without leaving
Cargo, sharing the socket client and the protocol DTO crate with the `sa` CLI. It is a *peer* of the bun
suite, not a replacement — the bun suite stays the canonical cross-engine parity harness (phase 3) and
the one that also proves the editor's client path. The Rust harness exists for engine-side regression
tests that are awkward to express in TypeScript (e.g. asserting on a strongly-typed DTO result, or a
test that wants to share a fixture with a unit test in the same crate).

Concretely:

- **A `saffron-e2e` dev/test crate** (or a `tests/` integration suite in the host crate) with a
  `TestEngine` that boots the Rust binary on a per-run control socket under headless weston, mirroring
  `harness.ts`'s `Engine` API in idiomatic Rust: `boot(env)`, `call::<R>(cmd, params) -> Result<R>`,
  `validation_errors() -> Vec<String>`, `settle(ms)`, `shutdown()`.
- **It reuses the `sa`-CLI socket client and the protocol crate** — the same framing (newline-delimited
  JSON, the `{id, cmd, params}` request / `{ok, result, error}` reply), the same `Uuid` decimal-string
  encoding, the same typed DTOs. No second wire implementation (NO LEGACY): the client is one crate the
  CLI, the Rust e2e harness, and any tooling share.
- **A small set of seed tests** that prove the harness, deliberately *not* duplicating bun coverage:
  ping/help/quit, a typed `render-stats` round-trip, and a `validation_errors()`-empty assertion on a
  cube scene. The bulk of behavioral coverage stays in bun; this proves the Rust path works and gives a
  template.
- **It honors `SAFFRON_ANIMA_BIN`** so it too can run against the C++ binary, making it usable in the
  parity rig (phase 7) for the typed-DTO comparisons.

## Why this shape (NO LEGACY)

- **One socket client, three consumers.** The C++ tree had the `sa` CLI socket code and the e2e
  TypeScript socket code as separate implementations. In Rust the client is a single crate shared by the
  `sa` CLI (`11-sa-cli`), this harness, and the parity rig — no duplicated framing, no chance of the
  test client and the production client drifting on the decimal-string-u64 encoding.
- **A peer, not a fork of the bun suite.** Re-implementing all 81 bun tests in Rust would be a second
  copy of validated coverage with no added detection (NO LEGACY). The Rust harness covers only what is
  *better* expressed in Rust (typed DTO assertions, crate-shared fixtures); the bun suite remains the
  source of truth for wire behavior and is the only one that exercises `@saffron/protocol` as the editor
  does.
- **Boot parity with the bun harness is required, not optional.** Both must use the same headless-weston
  + per-run-socket isolation so a test passing in one passes in the other against the same binary; the
  Rust harness copies `harness.ts`'s isolation model exactly rather than inventing a looser one.

## Grounding (real files/symbols)

- `tests/e2e/harness.ts` — the `Engine` API this mirrors: `boot` (weston + per-run socket isolation,
  `:60`), `call` (`:97`), `validationErrors` (`:54`), `settle` (`:159`), `shutdown` (`:186`); the env
  contract (`SAFFRON_ANIMA_BIN` `:17`, `SAFFRON_CONTROL_SOCK` `:79`, `WAYLAND_DISPLAY`/`SDL_VIDEODRIVER`
  `:77`).
- The shared socket client: the `11-sa-cli` socket-client phase (`phase-1-crate-and-socket-client`) and
  the protocol DTO crate (`10-protocol-codegen` phase 1) — the Rust e2e harness depends on both, not on
  a private wire impl.
- The envelope/framing contract: `engine-old/source/saffron/control/` socket dispatch (newline-framed,
  drain-once-per-frame) — cited by `09-control-plane` README; the Rust harness must frame requests
  identically.

## Acceptance gate

- `cargo test -p saffron-e2e` (or the host crate's `tests/`) boots the Rust binary under headless
  weston and passes: ping/help/quit, a typed `render-stats` round-trip deserializing into the protocol
  DTO, and a `validation_errors()`-empty assertion on a one-cube scene.
- The harness uses the same `sa`-CLI socket client crate and the same protocol DTO crate — no private
  JSON framing or `Uuid` encoding (`grep` shows no second `connect`/newline-framing impl outside the
  shared client).
- The harness honors `SAFFRON_ANIMA_BIN` and the same headless-weston isolation as `harness.ts`.
- `cargo test --workspace` green; clippy + fmt clean; the Cargo workspace compiles.
