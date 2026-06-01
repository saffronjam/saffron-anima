+++
title = 'se CLI'
weight = 2
+++

# se CLI

`se` is a small standalone binary that turns a shell command line into one JSON request, sends it over the [control socket](../control-plane-architecture/), and prints the reply. It links only `nlohmann_json` — no engine code, no `import std`, no Vulkan. That keeps it cheap to build and lets it talk to a running editor it knows nothing about.

## The request

`se <command> [positionals...] [--flag value] [-o text|json]` becomes a single line of JSON:

```json
{"cmd": "set-transform", "params": {"args": [123], "translation": {"x": 0, "y": 1, "z": 0}}, "id": 1}
```

The CLI splits argv into its own flags (`-o`/`--output`, `-h`), the command word, and the rest. Bare tokens go into a `params["args"]` array; `--key value` and `--key=value` become `params["key"]`; a bare `--key` with no value becomes `params["key"] = true`.

Every built-in reads its inputs through `positionalOr(params, "name", index)`, which returns `params["name"]` if present, else the index-th element of `params["args"]`, else null. So the same command accepts either form.

```sh
se set-aa msaa4          # positional → params["args"][0]
se set-aa --mode msaa4   # flag       → params["mode"]
```

## Token coercion

A bare token is typed by the client before it reaches the engine, in this order:

1. `true` / `false` / `null` → the JSON literal;
2. a token starting with `{`, `[`, or `"` → parsed as JSON (so you can pass an object inline);
3. an unsigned integer, then a signed integer, then a float;
4. otherwise a plain string.

So `se create-entity 42` sends the number `42` and `se create-entity Box` sends the string `"Box"`. Commands that need a specific type re-coerce defensively on their side — for example `set-material --albedoTexture` accepts a bare UUID string and converts it to a number so the component's `value<u64>` deserialize does not hit the `JSON_NOEXCEPTION` abort path.

## The reply, and output modes

The engine answers with one line: `{"ok": true, "result": {...}, "id": 1}` or `{"ok": false, "error": "...", "id": 1}`. On `ok:true` the CLI prints the result and exits 0; on `ok:false` it prints the error to stderr and exits 1. The non-zero exit lets a shell script branch on a failed command.

| Mode | Behaviour |
|---|---|
| `text` (default) | Human-readable. A handful of commands (`help`, `ping`, `list-entities`, `list-components`, `list-assets`, `render-stats`) get a one-line/table formatter; everything else falls through to pretty JSON with UTF-8 left unescaped (so an em dash renders as `—`). |
| `json` (`-o json`) | Raw `dump(2)` pretty JSON, ASCII-safe. Made for piping to `jq`. |

The output flag is an `se`-level concern, stripped before `params` is built, so the engine never sees it.

## Why a separate, dependency-free binary

The CLI proves the protocol is genuinely a wire contract, not a back door into engine internals: it cannot call an engine function even if it wanted to, only send a JSON line. The flip side is that adding a command needs no CLI change at all — an unknown command is just a request the engine resolves or rejects. The only reason to touch `se` is to add a prettier `text` formatter for a new reply, and even that is optional because the JSON fallback always prints.

## In the code

| What | File | Symbols |
|---|---|---|
| argv → request | `tools/se/source/main.cpp` | `splitArgs`, `buildParams`, `coerce` |
| Socket connect + round-trip | `tools/se/source/main.cpp` | `socketPath`, `main` |
| Reply printing | `tools/se/source/main.cpp` | `printResult`, `OutputMode` |
| Param reading, engine side | `command.cppm` | `positionalOr`, `asString` |

> [!NOTE]
> `cmd/se` in the repo root is a thin wrapper that runs the built binary inside the `saffron-build` toolbox; the host can reach the socket directly, so the wrapper is only a convenience. The binary itself is plain C++20.

## Related
- [Control plane](../control-plane-architecture/) — the server side of this protocol
- [Scene commands](../scene-commands/) · [Render commands](../render-commands/) · [Asset commands](../asset-commands/) — what you can ask it to do
