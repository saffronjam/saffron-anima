# tools/sa — Saffron Anima control CLI

A minimal C++20 binary (`source/main.cpp` plus the vendored header-only `args.hxx`)
that speaks JSON over the unix socket exposed by the running `SaffronAnima`. No
engine dependency — it only links `nlohmann_json` (`args.hxx` is header-only).

## Usage

```
sa <command> [positional...] [-o text|json] [--flag value|--flag=value]
```

`cmd/sa` in the repo root is a thin Python wrapper that runs the host-built binary
directly (`os.execv` of `build/debug/bin/sa`) — no toolbox involved, since the host
reaches the control socket directly. Only its wrapper-only `start` subcommand uses
`toolbox run -c saffron-build`, and that launches the *engine* (`SaffronAnima`, and
optionally `cmake --build` it), never the `sa` binary.

## Output modes (`-o` / `--output`)

| Mode | Behaviour |
|------|-----------|
| `text` (default) | Human-readable. `help` → two-column table. Everything else → pretty JSON with UTF-8 unescaped (em-dashes render correctly). |
| `json` | Raw pretty JSON, ASCII-safe (`dump(2)`). Good for piping to `jq`. |

The flag is stripped before building the params sent to the engine, so the engine
never sees it.

## Argument parsing

Top-level flags (`-o`/`--output`, `-h`/`--help`) are parsed by the vendored header-only
Taywee/`args` library (`args::ArgumentParser` + `args::MapFlag`). Everything after the
command name is coerced into engine params by the hand-rolled `buildParams`/`coerce`:

- Bare tokens → `params["args"]` array (positionals).
- `--key value` or `--key=value` → `params["key"]`.
- Bare `--key` (no following value) → `params["key"] = true`.
- Token coercion order: `true`/`false`/`null` literals → bool/null; JSON literal
  starting with `{`/`[`/`"` → parsed JSON; integer (unsigned then signed); float;
  fallback string.

## Adding a text formatter

Add a branch to `printResult()` in `source/main.cpp` keyed on `cmd`. The `result`
argument is the parsed JSON payload from the engine. Fall through to the UTF-8
`dump(2, ' ', false)` default for commands you do not handle explicitly.

## Build

```sh
toolbox run -c saffron-build bash -lc \
  'cd <repo> && cmake --build build/debug --target sa'
```

C++20 (not C++26 modules — `sa` has no `import std` and no engine headers).
