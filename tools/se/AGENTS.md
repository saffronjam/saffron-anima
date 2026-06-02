# tools/se — SaffronEngine control CLI

A minimal C++20 single-file binary (`source/main.cpp`) that speaks JSON over the
unix socket exposed by the running `SaffronEngine`. No engine dependency — it only
links `nlohmann_json`.

## Usage

```
se <command> [positional...] [-o text|json] [--flag value|--flag=value]
```

`cmd/se` in the repo root is a thin Python wrapper that invokes the built binary
via `toolbox run -c saffron-build` (not needed for the binary itself; the host can
reach the socket directly). It also adds a `start` subcommand that is wrapper-only.

## Output modes (`-o` / `--output`)

| Mode | Behaviour |
|------|-----------|
| `text` (default) | Human-readable. `help` → two-column table. Everything else → pretty JSON with UTF-8 unescaped (em-dashes render correctly). |
| `json` | Raw pretty JSON, ASCII-safe (`dump(2)`). Good for piping to `jq`. |

The flag is stripped before building the params sent to the engine, so the engine
never sees it.

## Argument parsing

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
  'cd /var/home/saffronjam/repos/SaffronEngine && cmake --build build/debug --target se'
```

C++20 (not C++26 modules — `se` has no `import std` and no engine headers).
