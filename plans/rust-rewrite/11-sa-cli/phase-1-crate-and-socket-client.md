# Phase 1 — the `sa` bin crate, the clap skeleton, and the socket round-trip

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-1-workspace-scaffold, 00-foundations:phase-2-core-crate, 10-protocol-codegen:phase-1-dto-crate-and-derives

## Goal

Stand up the `sa` `bin` crate as a workspace member depending only on `saffron-protocol` (engine-free),
with the `clap` (derive) argument skeleton (`-o/--output`, the trailing `<command> [args...]` capture)
and the complete `UnixStream` request-reply transport: resolve the socket path, build the request
envelope, send `<json>\n`, read one reply line, parse the envelope, and exit with the right code. After
this phase `sa ping` and `sa -o json ping` work end-to-end against a running walking-skeleton engine,
and the exit-code contract holds.

## Why this shape (NO LEGACY)

- **`clap` derive replaces the `args` + `splitArgs` pair.** The C++ needs both `Taywee/args` (for `-o`,
  `-h`) and a hand-rolled `splitArgs` (`main.cpp:501`) to peel CLI flags off before a free-form command,
  because `args` cannot model "options anywhere + an arbitrary command + free-form trailing args."
  `clap`'s derive expresses exactly that: a `struct Cli` with an `Option<OutputMode>` global flag and a
  `trailing_var_arg` + `allow_hyphen_values` positional that captures the command and its raw args. So
  `args.hxx` (5135 LOC) and `splitArgs` are both deleted; only `build_params` (phase-2) survives, because
  that is wire behavior, not argument parsing.
- **Engine-dependency-free is structural, not a guideline.** The crate's `Cargo.toml` lists
  `saffron-protocol` (and `clap`, `serde_json`, `rustix`, `anyhow`) — nothing from the engine
  subsystems. `saffron-protocol` depends only on `saffron-core` (foundations crate graph), so `sa` links
  no renderer, no Jolt, no Vulkan. This is what keeps it host-runnable outside the toolbox (README §1).
- **The transport is one blocking round-trip — no `rustix`, no tokio, no `unsafe`.** The client opens one
  socket, writes one line, reads one line, closes. `std::os::unix::net::UnixStream` covers it; the only
  syscall the C++ used directly that std abstracts is `getuid` for the fallback path, which comes from
  `rustix::process::getuid` (safe), so the crate holds `#![deny(unsafe_code)]`. There is no `MSG_NOSIGNAL`
  hack: a write to a vanished server returns `Err(BrokenPipe)` which `main` maps to exit 1; Rust does not
  raise `SIGPIPE` for a handled write error.
- **The reply is an opaque `Value`, not a typed DTO.** The CLI does not type results (README §4) — it
  reads only the envelope keys (`id`, `ok`, `result`, `error`). Typing the result would couple the CLI to
  every result DTO for no benefit and break the graceful-degradation the text formatters rely on. So the
  reply parses to `serde_json::Value`; the shared `saffron-protocol` types matter for `COMMANDS` (phase-4)
  and the `Uuid` derive, not for decoding the reply here.
- **Exit codes are a scriptable contract, ported exactly** (README §6): `0` ok, `1` runtime/connect/parse
  failure or an `ok:false` engine error, `2` missing command. `main` returns `anyhow::Result<()>` and a
  top-level match maps the outcome to `std::process::ExitCode` — anyhow is permitted in a `[[bin]]`
  (foundations idiom rules), and the top of the stack is where it belongs.

## Grounding (real files / symbols)

- `tools/sa/source/main.cpp`: `main` (533) — the envelope build (`request["cmd"]`/`["params"]`/`["id"]`,
  576), the `socket`/`connect`/`send`/`recv`/`close` round-trip (581–613), the reply parse + ok/error
  branch + exit codes (615–627); `socketPath` (26) — the frozen path resolution; `splitArgs` (501) and the
  `args::ArgumentParser`/`MapFlag`/`HelpFlag` setup (535–540) — the argument surface clap replaces.
- `engine-old/source/saffron/control/control_server.cpp`: `controlSocketPath` (160) — the server side of
  the same path rule (`SAFFRON_CONTROL_SOCK` → `$XDG_RUNTIME_DIR/saffron-control.sock` →
  `/tmp/saffron-control-<uid>.sock`); `dispatch` (226) — the reply envelope keys the CLI reads.
- `tools/sa/AGENTS.md`: the usage line `sa <command> [positional...] [-o text|json] [--flag value]` and
  the output-mode table (the `-o` flag is stripped before forwarding).
- `saffron-protocol` (`10-protocol-codegen` phase-1): the `Uuid` newtype + the DTO crate this links.

## Acceptance gate

- `cargo build -p sa` and `cargo build --workspace` succeed; the crate carries `#![deny(unsafe_code)]`;
  clippy + fmt clean. Its only engine-side dependency is `saffron-protocol` (asserted by inspecting the
  built dep graph — no `saffron-control`/`saffron-rendering`/`saffron-host` edge).
- A `#[test]` over `socket_path()` proves the three-way resolution: `SAFFRON_CONTROL_SOCK` wins when set;
  else `$XDG_RUNTIME_DIR/saffron-control.sock`; else `/tmp/saffron-control-<uid>.sock` with the real uid
  (env vars set/cleared within the test).
- A `#[test]` over the envelope builder proves the request serializes to `{"cmd":<name>,"params":<obj>,
  "id":1}` with `params` an object (empty object when no args), matching `main.cpp:576`.
- A `#[test]` over a reply-handling function proves: `{"ok":true,"result":{...}}` → exit 0 + the result
  passed to the printer; `{"ok":false,"error":"msg"}` → `sa: msg` on stderr + exit 1; a non-JSON reply →
  `sa: malformed reply` + exit 1; a `clap`-level missing-command → exit 2.
- An integration test (gated on a running engine, e.g. the walking-skeleton from `08-host-and-viewport`
  phase-3 or the e2e harness): `sa ping` connects to the live socket and prints a non-error reply (exit 0),
  and `sa -o json ping` emits parseable pretty JSON; `sa <garbage-command>` round-trips and exits 1 with
  the engine's `unknown command` error (proving the free-form forward).
