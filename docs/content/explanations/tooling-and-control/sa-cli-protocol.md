+++
title = 'sa CLI'
weight = 2
+++

# sa CLI

`sa` is a standalone command-line client that translates one shell invocation into one JSON request,
sends it over the [control socket](../control-plane-architecture/), and prints the reply. It is a
small Rust binary that links only `saffron-protocol` (the DTOs and the static command table) and
`saffron-control-client` (the shared wire client) — no renderer, no Jolt, no engine subsystem — so it
runs on the host outside the build toolbox and can drive a running editor it knows nothing about.

## How a command becomes a request

`sa <command> [positionals...] [--flag value] [-o text|json]` becomes a single line of JSON:

```json
{"cmd": "set-transform", "params": {"args": [123], "translation": {"x": 0, "y": 1, "z": 0}}, "id": 1}
```

`sa` parses its own surface with `clap`: a global `-o/--output`, the two built-in subcommands
(`start`, `completions`), and a free-form external arm that captures the control command and its
arguments verbatim. Because the control command flows through the external arm rather than a
per-command `clap` subcommand, a command is reachable the moment the engine registers it — `sa` never
needs a code change to forward a new command.

`build_params` then splits the captured tokens: bare tokens go into a `params["args"]` array;
`--key value` and `--key=value` become `params["key"]`; a bare `--key` with no value becomes
`params["key"] = true`. The shared `saffron-control-client` wraps the result in the request envelope.

The engine side folds positionals onto named fields. Every typed command knows its params DTO's
declaration order, so `args[i]` fills the `i`-th declared field when the named key is absent. The same
command therefore accepts either form:

```sh
sa set-aa msaa4          # positional → params["args"][0] → folds onto `mode`
sa set-aa --mode msaa4   # flag       → params["mode"]
```

## Token coercion

The client types each bare token before it reaches the engine, in this order:

1. `true` / `false` / `null` → the JSON literal;
2. a token starting with `{`, `[`, or `"` → parsed as JSON, so an object can be passed inline;
3. an unsigned integer (unless the token opens with `-`), then a signed integer, then a float;
4. otherwise a plain string.

The unsigned-first ordering is load-bearing: it keeps a large positive id (up to `u64::MAX`) an
unsigned number rather than lossily widening it to a float. So `sa create-entity 42` sends the number
`42` and `sa create-entity Box` sends the string `"Box"`. The typed DTO deserialize on the engine
side then validates each field against its declared type.

## The reply and output modes

The engine answers with one line: `{"ok": true, "result": {...}, "id": 1}` or `{"ok": false, "error":
"...", "id": 1}`. On `ok:true` the CLI prints the result and exits 0; on `ok:false` it prints the
error to stderr and exits 1; a usage error from `clap` exits 2. The non-zero exit lets a shell script
branch on a failed command.

| Mode | Behaviour |
|---|---|
| `text` (default) | Human-readable. A `match` over the command name gives many replies (`help`, `ping`, `list-entities`, `render-stats`, `raycast`, the profiler captures, …) a one-line/table formatter; everything else falls through to UTF-8-unescaped pretty JSON (so an em dash renders as `—`). |
| `json` (`-o json`) | `serde_json` pretty JSON, made for piping to `jq`. |

The output flag is an `sa`-level concern, stripped before `params` is built, so the engine never sees
it. Each text formatter is a pure function of the reply `Value`, so the arms are unit-tested directly.

## Discoverability and forwarding

`sa` never gates a command: an unknown name is still forwarded, and the engine answers `unknown
command '<name>'`. The CLI enriches that path two ways, both offline from the static
`saffron_protocol::COMMANDS` table:

- the long `--help` lists every registered command name and points at `sa help` for the live list, so
  the CLI is discoverable with no engine running;
- `sa completions <shell>` emits a shell-completion script whose candidate command list is the same
  static table; and when a forwarded command is absent from the table, the rendered engine error
  gains a nearest-name `did you mean '…'?` hint (Levenshtein-scored).

Because the table is the single source the runtime dispatch and the codegen both read, the static
help/completions cannot drift from what the engine actually serves.

## Launching the host

`sa start` is the one command that is not a socket round-trip: it launches the present-only host
(`saffron-host`) inside the `saffron-build` toolbox, detached by default or foreground under
`--attach`, optionally building it first with `--build` (`cargo build --bin saffron-host`). It skips
the launch when the engine is already up (unlinking a stale socket) and polls the socket for
readiness. The binary path is the workspace `target/<profile>/saffron-host`, overridable with
`SAFFRON_ANIMA_BIN` (the same parallel-binary knob the editor and e2e honor).

## In the code

| What | File | Symbols |
|---|---|---|
| Arg surface + dispatch | `engine/crates/sa/src/main.rs` | `Cli`, `Subcmd`, `main`, `forward` |
| argv → params | `engine/crates/sa/src/main.rs` | `build_params`, `coerce` |
| Reply printing | `engine/crates/sa/src/main.rs` | `print_result`, `format_text`, `OutputMode` |
| Help / completions / hints | `engine/crates/sa/src/main.rs` | `enriched_command`, `completion_command`, `did_you_mean` |
| Launcher | `engine/crates/sa/src/main.rs` | `start`, `engine_binary_path`, `poll_for_readiness` |
| Shared wire client | `engine/crates/control-client/src/lib.rs` | `Client`, `request_envelope`, `socket_path` |
| Positional-fold, engine side | `engine/crates/control/src/registry.rs` | `positional_or`, `fold_positional_args` |
| Static command table | `engine/crates/protocol/src/command.rs` | `COMMANDS`, `CommandSpec` |

> [!NOTE]
> The wire framing (the `<json>\n` request, the one reply line, the socket-path rule) lives in
> `saffron-control-client`, shared by the CLI and the e2e harness, so there is exactly one wire
> implementation in the tree. The CLI owns only its argument coercion and its text formatters.

## Related
- [Control plane](../control-plane-architecture/) — the server side of this protocol
- [Shared types](../shared-types/) — the DTO table the CLI links and the wire encoding it round-trips
- [Scene commands](../scene-commands/) · [Render commands](../render-commands/) · [Asset commands](../asset-commands/) — what you can ask it to do
