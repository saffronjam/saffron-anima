# tests/e2e — end-to-end engine tests

Black-box tests that boot a real `SaffronAnima` and drive it over the JSON-over-unix-socket
control plane — the same wire the editor and `sa` CLI use. The driver is plain TypeScript on
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
| `*.test.ts` | The suite (~46 files), grouped by area: control plane + rendering (`rendering`, `control`, `scene`, `camera`, `picking`, `play`, `perf`, `profiler`, `toggles`, `assets`, `hierarchy`, …), animation (`animation*`, `foot-ik`), skinning (`skinning`, `skinned-*`, `skeleton-overlay`), scripting (`script`), materials (`material*`), and pixel/golden render checks (`*_render`, `material_scene_codegen`). |

## Conventions

- **No display setup needed.** Each `Engine` starts its own headless weston with a unique
  socket, so tests are isolated and never open a window. Needs `weston` + the engine binary
  (build it first: `make engine`).
- **Assert on `validationErrors()`.** The engine runs with validation layers on; a test that
  exercises a feature should assert the log stays free of `[saffron:vulkan] error: [validation]`
  lines — that is what catches GPU-state bugs (e.g. the MSAA sample-count regression) headlessly.
- **Two tiers.** Behavioral/state tests assert on control responses + `validationErrors()` (zero-dep).
  Pixel tests drive the `screenshot` command (`target: "viewport"`, a path), wait for the PNG, read it
  back, and compare buffers directly (`Buffer.equals`, no image-diff dep) — see the `*_render.test.ts`
  files. Golden-image baselines are not wired up yet.
- Type results via `@saffron/protocol` (`engine.call<RenderStats>("render-stats")`) so a schema
  change that breaks an assertion shows up at typecheck.
