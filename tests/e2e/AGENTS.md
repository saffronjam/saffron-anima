# tests/e2e — end-to-end engine tests

Black-box tests that boot a real `SaffronEngine` and drive it over the JSON-over-unix-socket
control plane — the same wire the editor and `se` CLI use. The driver is plain TypeScript on
`bun test`; nothing here is C++. The wire contract is consumed through the generated
`@saffron/protocol` types (from `schemas/control/`), so assertions stay in sync with the schema.

```sh
make e2e                       # from anywhere — auto-enters the toolbox
cd tests/e2e && bun test       # inside the toolbox (host bun on PATH)
```

## Layout

| File | Role |
|---|---|
| `harness.ts` | `Engine.boot()` spawns a headless weston + the engine on a per-run control socket, captures stdout/stderr into `.log`, and exposes `call(cmd, params)` + `validationErrors()`. Always `shutdown()`. |
| `rendering.test.ts` | Control-plane + rendering cases (boot-clean, model import, MSAA validation-clean regression). |

## Conventions

- **No display setup needed.** Each `Engine` starts its own headless weston with a unique
  socket, so tests are isolated and never open a window. Needs `weston` + the engine binary
  (build it first: `make engine`).
- **Assert on `validationErrors()`.** The engine runs with validation layers on; a test that
  exercises a feature should assert the log stays free of `[ERROR: Validation]` — that is what
  catches GPU-state bugs (e.g. the MSAA sample-count regression) headlessly.
- **Two tiers.** Behavioral/state tests work today (zero-dep). Pixel/golden-image tests need the
  `screenshot` control command to actually write a file first, plus an image-diff dependency.
- Type results via `@saffron/protocol` (`engine.call<RenderStats>("render-stats")`) so a schema
  change that breaks an assertion shows up at typecheck.
