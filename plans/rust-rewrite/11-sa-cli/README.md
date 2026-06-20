# 11 — `sa` CLI: the native control client over the frozen socket

`sa` is the shell-facing control client: a single binary that opens the engine's unix socket, sends one
JSON envelope, reads one reply, prints it, and exits. It is how a developer drives and inspects a
running `SaffronAnima` from a terminal — the scriptable, visually-debuggable seam every engine feature
gets a matching control command for (`AGENTS.md`). This area ports the C++ `tools/sa` (a 628-LOC
`main.cpp` over the vendored 5135-LOC header-only `Taywee/args`) into the **`sa`** Cargo `bin` crate,
built on `clap` (derive) for the argument surface and on **`saffron-protocol`** for the shared wire
types so the CLI and the engine can never drift.

The crate is deliberately the **leaf-most consumer of the wire**: it depends on `saffron-protocol`
**only** (the foundations crate graph fixes this edge) — no `saffron-control`, no renderer, no engine
subsystem. It links the same `CommandSpec` table and the same DTO/`Uuid` derives the engine's handlers
use, so `sa help`, the command set, and the id encoding are generated from one source. It stays
**host-runnable** (the toolbox lore: `sa` reaches the control socket directly from the Silverblue base
OS, outside the `saffron-build` container) — pure-Rust, `#![deny(unsafe_code)]`, no C++ toolchain, no
Vulkan, no Jolt.

Read this with [`09-control-plane/catalog.md`](../09-control-plane/catalog.md) (the 153-command surface
the CLI mirrors), [`10-protocol-codegen/README.md`](../10-protocol-codegen/README.md) (the
`saffron-protocol` crate + `CommandSpec` table this CLI shares), and
[`00-foundations/conventions.md`](../00-foundations/conventions.md) (the idiom rules). The C++ reference
is `tools/sa/source/main.cpp` and `tools/sa/AGENTS.md` (note: `tools/sa` lives at the repo root, **not**
under `engine-old/`).

---

## 1. What the CLI is (and the one thing it must reproduce exactly)

The CLI is a thin, stateless request-reply transport with a presentation layer. Its job:

1. Parse `sa <command> [positional...] [-o text|json] [--flag value | --flag=value | --flag]`.
2. Build the request envelope `{ "cmd": <command>, "params": <coerced>, "id": 1 }`.
3. Connect to the control socket, send `<json>\n`, read until the first `\n`, close.
4. Parse the reply envelope `{ "id", "ok", "result" | "error" }`; on `ok` print the result (text or
   raw JSON), on `!ok` print the error to stderr; set the exit code.

The **one wire behavior the CLI itself originates** (everything else is the engine's) is the
**param-coercion + positional/flag mapping** in `buildParams`/`coerce` (`main.cpp:40`,`:87`): bare
tokens become `params.args[]`, `--key value`/`--key=value` become `params[key]`, a bare `--key` becomes
`params[key] = true`, and each value is coerced by a fixed precedence (`true`/`false`/`null` literals →
JSON-literal-if-it-starts-`{`/`[`/`"` → unsigned int → signed int → float → bare string). This is the
exact pre-pass that lets `sa set-camera 1 2 3` and `sa set-camera --yaw 90` both reach the typed handler
— it is the client mirror of the server-side lenient/positional read (area-09 README §6), and it must be
**byte-identical** because the engine's handlers parse what this produces. It ports verbatim as a
unit-tested pure function, not re-derived.

`sa` is otherwise dumb: it does **not** know any command's params shape (it forwards a coerced `Value`),
and it does **not** validate against the schema. That keeps it engine-free and means a new control
command is reachable through `sa` the moment the engine registers it — no CLI change. The `CommandSpec`
table is used for `help`/completions/spell-check only, never to gate a forward.

## 2. Why `clap` + `saffron-protocol`, and what each owns

The C++ split is awkward: `Taywee/args` parses only the two CLI-level flags (`-o`, `-h`), and a
hand-rolled `splitArgs` (`main.cpp:501`) walks argv to peel those flags off *before* the command and
*before* the engine args, because `args` cannot model "flags anywhere + an arbitrary trailing command +
free-form engine args." The Rust port keeps that separation but expresses it idiomatically:

| Concern | C++ | Rust | Owns |
|---|---|---|---|
| CLI-level flags (`-o/--output`, `-h/--help`, `start`) | `args::ArgumentParser` + `splitArgs` peel | `clap` derive `struct Cli` with `#[command(...)]` | this crate |
| The command name + free-form engine args | `splitArgs` + `buildParams` | a `clap` trailing-var-arg capture, then `build_params` | this crate |
| The wire types (`Uuid` decimal-string, DTO shapes) | `nlohmann::json` + `JSON_NOEXCEPTION` | `saffron-protocol` (`serde_json::Value` + `Uuid`) | `saffron-protocol` |
| The command list (names + summaries for `help`) | the engine's live `help` reply | `saffron_protocol::COMMANDS` (`&'static [CommandSpec]`) | `saffron-protocol` |

**`clap` over a hand-rolled splitter (NO LEGACY).** `splitArgs` exists only because `args` is too rigid;
`clap`'s derive models exactly this shape — top-level options plus a `trailing_var_arg`/
`allow_hyphen_values` positional capture — so the bespoke argv walk and the entire vendored 5135-LOC
`args.hxx` are deleted with no replacement. `clap` gives `-h/--help`, usage, error messages, and shell
completions for free.

**`saffron-protocol` is the only engine-side coupling, and it is a `lib` with no engine deps** (it
depends on `saffron-core` alone — foundations crate graph), so linking it does **not** drag the renderer
or Jolt into `sa`. The CLI gets the frozen `Uuid` decimal-string derive and the `CommandSpec` table from
the same crate the engine uses, which is the whole "never drift" guarantee: there is no second command
list, no second id encoding.

## 3. The command surface: forwarded, not enumerated

The C++ CLI does **not** declare one subcommand per control command — it takes the command name as a
free-form positional and forwards it. The Rust port keeps this (it is what makes the CLI engine-free and
forward-compatible), so the `clap` shape is **one** top-level binary with:

- Global options: `-o/--output <text|json>` (a `clap` `ValueEnum`, default `text`); `-h/--help`,
  `-V/--version` (free from `clap`).
- One built-in subcommand: `start [--attach] [--build]` — the engine launcher (§7), the only piece that
  is not a socket round-trip.
- An `external_subcommand`/trailing capture: `<command> [args...]` where `command` is any string and
  `args` is the raw, un-validated remainder (with `allow_hyphen_values` so `--flag` tokens survive into
  `build_params`).

So `sa` does not grow a 153-arm subcommand enum. The 153 commands surface through:

- **`sa help`** — sent to the engine, which returns `{ commands: [{ name, help }] }` (the live reflective
  registry); the text formatter prints the two-column table. This is the authoritative list.
- **`sa --help`** (clap) — prints the binary's own usage (options + `start` + the `<command> [args...]`
  shape) and, in the long help, a pointer to `sa help` for the command list. The crate may *additionally*
  enrich clap's help from `saffron_protocol::COMMANDS` (the static table — names + summaries), so
  `sa --help` lists commands even with no engine running; but `sa help` (the live reply) stays the
  primary, because the running engine is the source of truth for what it actually serves.
- **Shell completions** — generated from `saffron_protocol::COMMANDS` (names) via `clap_complete`, an
  affordance the C++ CLI never had (the static table makes it free).

The `CommandSpec` table is therefore used for **discovery** (help enrichment, completions, an optional
"did you mean…?" on an unknown command), never to *restrict* what can be sent — an unknown command is
still forwarded and the engine answers `unknown command '<name>'`, preserving the C++ behavior exactly.

## 4. The socket client: same framing as the server, no async

The transport ports 1:1 from `main.cpp:580`–`613` onto `std::os::unix::net::UnixStream` (std is enough;
no `rustix`, no tokio — the client does one blocking round-trip):

- **Path resolution** is the frozen rule (identical to the server's `controlSocketPath` and to the C++
  CLI's `socketPath`, `main.cpp:26`): `SAFFRON_CONTROL_SOCK` if set, else
  `$XDG_RUNTIME_DIR/saffron-control.sock`, else `/tmp/saffron-control-<uid>.sock`. The `<uid>` is the
  real uid (`getuid`), read via `rustix::process::getuid` (the crate already pinned for the host shm
  seam) or `std`’s `unsafe`-free path — but since this crate is `#![deny(unsafe_code)]`, the uid comes
  from `rustix` (safe), not a raw libc call.
- **Connect**: `UnixStream::connect(path)`; on failure print `sa: cannot connect to <path>: <err>` to
  stderr and exit 1 (matching `main.cpp:591`).
- **Send**: `request.to_string()` + `'\n'`, written with `write_all`. The C++ uses `MSG_NOSIGNAL` to
  avoid `SIGPIPE` on a vanished server; the Rust port gets this for free — `std::io` write errors are
  `Result`, and Rust's default `SIGPIPE` disposition for a process that handles the error is benign
  (the write returns `BrokenPipe`, no `unsafe` signal hack needed).
- **Receive**: read into a buffer until the first `\n` (the reply is one line); the engine's flush loop
  guarantees the whole line arrives. Mirror `main.cpp:602`–612 — accumulate `recv` chunks until a
  newline is seen, then stop.
- **Parse**: `serde_json::from_str::<Value>` on the reply; a malformed reply → `sa: malformed reply` on
  stderr, exit 1 (`main.cpp:615`).

There is no second transport and no length-prefix variant (the wire is frozen newline-delimited JSON;
area-09 §7). The reply is read as an opaque `serde_json::Value` because the CLI does not type the result
— the typed DTOs matter on the engine side; the CLI only needs the envelope keys (`id`, `ok`, `result`,
`error`) and whatever fields a text formatter reads.

## 5. Output modes and the text formatters

Two modes, identical to the C++ `OutputMode` (`main.cpp:20`):

- **`json`** — `serde_json::to_string_pretty(&result)` (the `dump(2)` analogue), ASCII-safe, for piping
  to `jq`. The `-o` flag is consumed by the CLI and **never** forwarded to the engine (the C++ strips it
  in `splitArgs` before `buildParams`).
- **`text`** (default) — human-readable. `help` prints a two-column table; ~35 commands have a bespoke
  one-line formatter keyed on the command name; everything else falls through to pretty JSON with UTF-8
  **unescaped** (`dump(2, ' ', false)` — so em-dashes render as `—`, not `—`). `serde_json`'s
  pretty printer does not escape non-ASCII by default, so the fallback is `to_string_pretty`.

The **35 command-keyed text formatters** in `printResult` (`main.cpp:127`–489) port verbatim — they are
a closed `match cmd { ... }` over the command-name string, each arm reading specific result fields with
lenient defaults (the `result.value("key", default)` pattern → a small `Value` field-reader helper:
`obj.get(k).and_then(...).unwrap_or(default)`). They are pure presentation over a `&Value`, so they are
straightforward `#[test]`-covered functions. The exact set (the arms to reproduce, grouped):

- **Lists / tables**: `help`, `list-entities`, `list-components`, `list-assets`, `list-clips`,
  `get-asset-model` (the bone-tree indenting walk + clip list), `pass-timings`, `drain-alarms`,
  `list-active-alarms`, `drain-contacts`.
- **One-line status**: `ping`, `render-stats`, `profiler.set-mode`, `frame-history`,
  `get-perf-config`/`set-perf-config`, `play`/`pause`/`stop`/`step`/`get-play-state`, `physics-state`,
  `set-kinematic-bones`, `set-active-view`, `add-entity`/`copy-entity`, `get-gizmo`/`set-gizmo`,
  `gizmo-pointer`, `get-camera`/`set-camera`, `viewport-native-info`.
- **Hit / found / present branches**: `raycast`/`shapecast`, `pick`, `pick-skeleton-joint`,
  `enable-ragdoll`/`set-ragdoll`/`get-ragdoll`, `move-character`, `fit-collider`, `get-selection`.
- **Special side-effect formatting**: `profiler.capture-start` (prints the capture id + stop hint),
  `profiler.capture-stop` (frame/span counts, and writes the inline Chrome-Trace JSON to a temp file when
  no `path` is in the reply — `std::env::temp_dir()` + `saffron-profile.json`, `main.cpp:268`),
  `get-thumbnail`/`view-asset` (decodes the base64 length into an approximate byte count).

These formatters are the CLI's only domain knowledge of result shapes, and they degrade gracefully (a
missing field reads its default), so they never break when the engine adds a field — matching the C++
`.value(key, default)` leniency. **Adding a formatter is one `match` arm** (the C++ `AGENTS.md` rule
"add a branch to `printResult` keyed on `cmd`"), preserved.

## 6. Exit codes (the frozen contract)

The C++ exit codes port exactly (they are a scriptable contract — callers branch on them):

| Code | When | C++ site |
|---|---|---|
| `0` | `ok: true` reply printed; or `--help`/`start` success | `main.cpp:551`,`:624` |
| `1` | socket/connect failure, malformed reply, **or** an `ok: false` engine error (printed to stderr) | `:557`,`:585`,`:593`,`:619`,`:626` |
| `2` | missing command (no positional command given) | `:569` |

The error message for an `ok: false` reply is `sa: <error>` on stderr, the engine's `error` string
verbatim (`main.cpp:626`). A `clap` parse error (unknown option) exits with `clap`'s convention (2) and
its own message — this is *new* (the C++ `args::ParseError` path also exited 1/printed help), so the
port aligns clap's `ErrorKind` exit to keep `2 = usage error` and `1 = runtime error` coherent: clap's
default usage-error code is 2, which matches "missing command," so the mapping is consistent.

`bin` crates may use `anyhow` (foundations: anyhow allowed only in `[[bin]]`/xtask), so `main` returns a
`Result` and maps the error to the right exit code via a top-level match — the typed-error discipline is
a library concern; the CLI's `main` is the top of the stack.

## 7. The `start` subcommand: the engine launcher (the `cmd/sa` wrapper folded in)

Today the host-side Python wrapper `cmd/sa` owns `start` (launch `SaffronAnima` in the toolbox,
optionally `cmake --build` first, optionally `--attach`), and delegates every other command to the
compiled binary via `os.execv` (`cmd/sa:46`,`:95`). The Rust port **folds `start` into the `sa` binary
itself** (NO LEGACY: one `sa`, not a binary plus a Python wrapper that re-execs it). `sa start`:

- `--build` → run the engine build first (the Cargo/justfile build, replacing the `cmake --build` call —
  the exact build command is the `01-build-and-toolchain` recipe, invoked as a subprocess).
- check whether the engine is already up (connect to the socket; if the path exists but refuses, unlink
  it — `cmd/sa:23`).
- launch the engine binary inside the toolbox (`toolbox run -c saffron-build <engine-bin>`), detached
  (`--attach` keeps it in the foreground), then poll the socket for readiness (`cmd/sa:68`).

This is the **only** subcommand that is not a socket round-trip and the only one that shells out to the
toolbox; it stays a thin `std::process::Command` wrapper. The engine-binary path and the toolbox
invocation are the `01-build-and-toolchain` contract; `sa` consumes them. Everything else (`sa <any
control command>`) is the pure socket client of §4 — so the single `sa` binary subsumes both `cmd/sa`
(the wrapper) and `build/debug/bin/sa` (the C++ binary), and `cmd/sa` is deleted.

## 8. Subtractions (NO LEGACY)

- **The vendored `tools/sa/args.hxx` (5135 LOC, `Taywee/args`) is deleted** — `clap` replaces the whole
  argument surface; no vendored CLI parser.
- **`splitArgs` (the hand-rolled argv pre-walk, `main.cpp:501`) is deleted** — `clap`'s derive models
  "global flags + trailing command + free-form args" directly; the only argv logic that survives is
  `build_params`/`coerce` (the param coercion, which is wire behavior, not argument parsing).
- **The C++ `OutputMode` `MapFlag` + the `JSON_NOEXCEPTION` `nlohmann` dependency are deleted** — `clap`
  `ValueEnum` for `-o`, `serde_json` for the wire (returns `Result`, no abort firewall).
- **The Python `cmd/sa` wrapper is deleted** — `start` folds into the `sa` binary (§7); there is one
  `sa`, host-runnable, no Python.
- **The `-stdlib=libstdc++` / libc++-vs-libstdc++ split (`tools/sa/CMakeLists.txt`) is deleted** — a Rust
  binary has no C++ runtime to pick; it links the Rust std and `saffron-protocol`, runs on the host
  directly.
- **No second command list, no hand-synced enum of subcommands** — the command set is forwarded
  free-form and discovered through `sa help` (live) + `saffron_protocol::COMMANDS` (static), one source.

## 9. The shared-protocol guarantee (the "never drift" mechanism)

The reason this area exists alongside `10-protocol-codegen`: by linking `saffron-protocol`, the CLI
inherits the same `Uuid` decimal-string derive and the same `CommandSpec` table the engine's handlers
and the OpenRPC/manifest emitters use. So:

- An id the CLI coerces and sends, and an id the engine emits, use the **same** `Uuid` encode/decode —
  there is no second decimal-string implementation in the CLI (the C++ CLI had `nlohmann` re-parse it;
  the Rust CLI shares the newtype). In practice the CLI forwards ids as opaque coerced `Value`s, so the
  shared `Uuid` matters most for any CLI-side typing of a result; the binding is what makes a future
  typed-CLI path drift-proof.
- `sa help`'s command names and the engine's registered commands cannot diverge, because completions and
  help-enrichment read `saffron_protocol::COMMANDS`, the **same** static table the runtime
  `register_typed` calls iterate (10-protocol-codegen phase-4).

A `#[test]` in this crate asserts the CLI's view of `COMMANDS` is non-empty and contains the known
anchors (`ping`, `quit`) — a tripwire that the protocol crate edge is live; the byte-identity of the
`Uuid` encoding is already proven by the protocol crate's own cross-encoder test (10-protocol-codegen
phase-1), so this crate does not re-test it.

## 10. The phase split

Four phases, each leaving the workspace green. The crate skeleton + transport land first (a `sa ping`
that talks to the walking-skeleton engine is the earliest end-to-end proof); the param coercion and the
text formatters are pure-function increments tested without a running engine; the `start` launcher lands
last because it depends on the `01-build-and-toolchain` build recipe + engine-binary path.

| Phase | What | Depends on |
|---|---|---|
| `phase-1-crate-and-socket-client` | the `sa` bin crate, `clap` skeleton (`-o`, command + trailing args), the `UnixStream` round-trip, envelope build/parse, exit codes, `ping`/raw-JSON output | `00-foundations`, `10-protocol-codegen:phase-1-dto-crate-and-derives` |
| `phase-2-param-coercion` | `build_params` + `coerce` ported verbatim as tested pure functions (positional/flag mapping + the coercion precedence) | `phase-1` |
| `phase-3-text-formatters` | the `text` mode: the `help` table + the ~35 command-keyed formatters + the capture-stop temp-file write + the UTF-8-unescaped fallback | `phase-1`, `phase-2` |
| `phase-4-help-completions-and-start` | `saffron_protocol::COMMANDS` help-enrichment + `clap_complete` completions + the `start` engine-launcher subcommand (folds `cmd/sa`) | `phase-1`, `10-protocol-codegen:phase-4-command-table`, `01-build-and-toolchain` |

## 11. Grounding (real files / symbols)

| What | File | Symbols |
|---|---|---|
| The CLI being replaced (whole binary) | `tools/sa/source/main.cpp` | `main`, `socketPath`, `coerce`, `buildParams`, `printResult`, `splitArgs`, `OutputMode` |
| Vendored arg parser (deleted) | `tools/sa/args.hxx` | `args::ArgumentParser`, `args::MapFlag`, `args::HelpFlag` (replaced by `clap`) |
| Build wiring (deleted) | `tools/sa/CMakeLists.txt` | the `sa` target, `JSON_NOEXCEPTION`, `-stdlib=libstdc++` |
| CLI behavior notes (the contract to preserve) | `tools/sa/AGENTS.md` | usage line, output-mode table, arg-parsing precedence, "add a text formatter" rule |
| The Python wrapper (folded into `start`) | `cmd/sa` | `cmd_start`, `cmd_build`, `is_engine_running`, `socket_path` |
| Socket path resolution (frozen, must match) | `engine-old/source/saffron/control/control_server.cpp` | `controlSocketPath` (160) |
| Envelope reply shape (the keys the CLI reads) | `engine-old/source/saffron/control/control_server.cpp` | `dispatch` (226): `id` echo, `ok`, `result`/`error` |
| The shared command table | `saffron-protocol` (`10-protocol-codegen` phase-4) | `COMMANDS: &'static [CommandSpec]` (`{ name, summary, params, result }`) |
| The shared `Uuid` decimal-string newtype | `saffron-protocol` (`10-protocol-codegen` phase-1) | `Uuid(u64)` + `serde_with::PickFirst<(DisplayFromStr, _)>` |
| The 153-command surface the CLI mirrors | `09-control-plane/catalog.md` | the full command list + the wire-helper encodings |
