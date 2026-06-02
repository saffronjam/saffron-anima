# Saffron.Control

The control plane: a JSON-over-unix-socket server that lets the Tauri editor (and the
`se` CLI) drive and inspect the running host. Module `Saffron.Control`, partition
`:Command`, namespace `se`. Uses classic `#include` in the global module fragment and
does **not** `import std`.

## Files

| File | Role |
|---|---|
| `command.cppm` | Partition `:Command`. The public types: `CommandTraits`, `CommandRegistry`, `EngineContext` (borrowed `{window, renderer, sceneEdit, assets}`), `ControlClient`, `ControlServer`, `ControlContext`, plus `registerCommand`. |
| `control.cppm` | Outer module, re-exports `:Command`. |
| `control_server.cpp` | Socket setup, dispatch, the per-frame non-blocking drain. |
| `control_commands_render.cpp` | Render stats, AA/clustering/GI toggles, `attach-native-viewport` / `resize-native-viewport` (the X11 reparent). |
| `control_commands_scene.cpp` | Entity lifecycle, components, selection, picking, camera, gizmo, environment, `dump-schema`. |
| `control_commands_asset.cpp` | Import, catalog, thumbnails, project/scene save & load. |

## Protocol

Newline-delimited JSON over a unix socket. Path resolution: `SAFFRON_CONTROL_SOCK` if
set, else `$XDG_RUNTIME_DIR/saffron-control.sock`, else `/tmp/saffron-control-<uid>.sock`
(mode 0600).

```
request:  { "id": <opt>, "cmd": "<name>", "params": { … } }\n
response: { "id": <echoed>, "ok": true|false, "result": { … } | "error": "<msg>" }\n
```

The envelope is defined by `schemas/control/envelope.schema.json`. The server is drained
once per frame and **never blocks** (non-blocking accept/read/send).

## Adding a command

```cpp
registerCommand(registry, "my-command", "one-line help",
  [](EngineContext& ctx, const json& params) -> Result<json>
  {
    // ctx.{window,renderer,sceneEdit,assets} are borrowed — valid only for this call.
    return json{ {"ok-field", 1} };   // success payload
    // return Err("why it failed");   // failure
  });
```

Conventions to follow:

- **Parameters accept named or positional form.** Use the `positionalOr(params, name,
  index)` helper so `--flag value` and bare positional args both work (the latter arrive
  in `params["args"]`).
- **Entity selectors resolve by UUID or exact name** via `resolveEntity`. IDs are u64 —
  serialize them as **strings** so JS keeps full precision.
- **Schema-first contract.** Anything new in the wire format gets a hand-authored schema
  in `schemas/control/` (see that directory's `AGENTS.md`). `dump-schema` must keep
  reflecting the live component/environment/render-stats DTOs, and the contract test in
  `tools/check-control-schema/` validates live output against the schemas.
- **Keep the `se` CLI usable.** A command that adds drivable/inspectable state should also
  be reachable from `tools/se` — that is part of "done" per the root `AGENTS.md`.
