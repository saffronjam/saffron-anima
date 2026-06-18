# Phase 4 — `COMMANDS` help-enrichment, shell completions, and the `start` engine launcher

**Status:** COMPLETED

**Depends on:** 11-sa-cli:phase-1-crate-and-socket-client, 10-protocol-codegen:phase-4-command-table, 01-build-and-toolchain

## Goal

Close the CLI by wiring in the three pieces that need the shared `CommandSpec` table or the build
toolchain: (1) enrich `clap`'s `--help` and add an optional "did you mean…?" hint from
`saffron_protocol::COMMANDS` (so the CLI lists commands even with no engine running); (2) generate shell
completions from the same table via `clap_complete`; (3) fold the `cmd/sa` Python wrapper's `start`
subcommand into the binary (launch the engine in the toolbox, optional `--build`, optional `--attach`,
poll the socket for readiness). After this phase the single `sa` binary subsumes both the C++ binary and
the Python wrapper, and `cmd/sa` is deleted.

## Why this shape (NO LEGACY)

- **`COMMANDS` is the shared table — the "never drift" mechanism made concrete.** The CLI reads
  `saffron_protocol::COMMANDS` (the `&'static [CommandSpec]` from 10-protocol-codegen phase-4 — the same
  ordered table the engine's `register_typed` calls iterate and the OpenRPC/manifest emitters read) for
  help-enrichment and completions. There is no second command list in the CLI (README §3/§9): the live
  `sa help` reply is the runtime source of truth, and `COMMANDS` is the static source for offline
  discovery — both derive from the one table, so they cannot diverge. `sa help` (the socket round-trip,
  already handled by phase-3's table formatter) stays the primary, authoritative list.
- **Completions are a free affordance the C++ CLI never had.** `clap_complete` generates a completion
  script from the clap `Command`; the command *names* come from `COMMANDS` (registered as possible values
  for the trailing command, or emitted into the completion candidates). This is purely additive over the
  static table — no engine contact — so it is offline and drift-proof. A `sa completions <shell>`
  subcommand (or a hidden flag) emits the script.
- **`start` folds the Python wrapper into the one binary (NO LEGACY: one `sa`).** The C++ tree has two
  artifacts: `build/debug/bin/sa` (the binary) and `cmd/sa` (a Python wrapper that owns `start` and
  `os.execv`s the binary for everything else, `cmd/sa:46`,`:95`). The Rust port deletes the wrapper:
  `sa start` is a subcommand of the binary itself, a thin `std::process::Command` orchestrator. The
  delegation indirection disappears because there is no separate binary to re-exec — `sa <command>` and
  `sa start` are arms of the same `clap` parse.
- **`start` is the only non-socket subcommand and the only toolbox shell-out.** Its steps port from
  `cmd_start` (`cmd/sa:46`): `--build` runs the engine build first (the `01-build-and-toolchain` build
  recipe — `cargo build`/the justfile target — replacing the C++ `cmake --build`, `cmd/sa:37`); check
  if the engine is already up by connecting to the socket and `unlink`ing a stale path
  (`is_engine_running`, `cmd/sa:23`); launch `toolbox run -c saffron-build <engine-bin>` detached
  (`Stdio::null`), or in the foreground under `--attach`; then poll the socket path for up to ~5s
  (`cmd/sa:68`) printing readiness. The engine-binary path and toolbox invocation come from the
  `01-build-and-toolchain` contract; this phase consumes them rather than re-deciding them. It stays
  `#![deny(unsafe_code)]` — `std::process` + the phase-1 socket connect, no raw `execv`.
- **The build-toolchain dependency is why `start` lands last.** `start --build` invokes the workspace
  build, and the engine-binary path is the build output location — both are owned by
  `01-build-and-toolchain`. The socket-only commands (phases 1–3) have no such dependency and ship
  earlier; `start` is the one piece that needs the build story settled.

## Grounding (real files / symbols)

- `cmd/sa` (the Python wrapper, deleted/folded): `cmd_start` (46) — the already-running check, the
  detached launch, the readiness poll; `cmd_build` (37) — the `toolbox run … cmake --build` (replaced by
  the Cargo build); `is_engine_running` (23) — connect + unlink-stale; `socket_path` (15) — the same
  frozen resolution as phase-1; the `start`-vs-delegate dispatch in `main` (82–95).
- `saffron-protocol` (`10-protocol-codegen` phase-4): `COMMANDS: &'static [CommandSpec]` (`{ name,
  summary, params, result }`) — names + summaries for help-enrichment and completions, in registration
  order.
- `01-build-and-toolchain`: the engine build recipe + the engine-binary path + the toolbox invocation
  (`toolbox run -c saffron-build`) `start` shells out to.
- `tools/sa/source/main.cpp`: the `args::HelpFlag`/usage output (537,550) `clap`'s `--help` replaces; the
  `start` subcommand is *not* in the C++ binary (it lived only in the Python wrapper) — this phase adds it.

## Acceptance gate

- `cargo build --workspace` succeeds; clippy + fmt clean; `#![deny(unsafe_code)]` holds.
- A `#[test]` asserts `saffron_protocol::COMMANDS` is reachable from this crate, is non-empty, and
  contains known anchors (`ping`, `quit`) — the tripwire that the protocol-crate edge is live (README §9).
- A `#[test]` proves help-enrichment: the long `--help` text (rendered from the clap `Command`) lists at
  least the `COMMANDS` anchor names and points at `sa help` for the live list; this works with **no**
  engine running (offline, from the static table).
- A `#[test]` generates a completion script (`clap_complete` for one shell, e.g. bash) and asserts it
  contains a command anchor name (`ping`) — proving completions are sourced from `COMMANDS`.
- A `#[test]` over the unknown-command path asserts a "did you mean…?" hint is computed against `COMMANDS`
  by nearest-name (optional behavior), and that an unknown command is still **forwarded** to the engine
  (not blocked) — preserving the C++ free-form behavior (the engine answers `unknown command`).
- An integration test (gated on the toolbox/build being available, else skipped+logged): `sa start`
  launches the engine, polls the socket, and a subsequent `sa ping` succeeds; `sa start` again reports
  "already running" (exit 0). `cmd/sa` no longer exists in the tree (a repo check), and the single `sa`
  binary handles both `start` and every control command.
