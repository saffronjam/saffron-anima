# Phase 4 — export cook + staging

**Status:** COMPLETED — `AppManifest` + `ExportAppParams`/`ExportAppResult` DTOs in
`saffron-protocol` (regenerated artifacts); `export-app` joined the frozen 154→155 command table
(domain/count tests + skip metadata updated); handler in `saffron-control` (`commands_asset.rs`)
pre-bakes every material's mesh `.spv` (reusing the `material-cook` loop) then stages
`saffron-player` + `assets/` + `src/` + engine `shaders/` + `project.json` + a written `app.json`.
Verified: workspace build + `clippy -D warnings` + protocol tests clean; an `export-app` e2e
(`tests/e2e/export-app.test.ts`) exports the project, asserts the staged folder is complete + the
`app.json` round-trips, and boots the exported player against the folder validation-clean (passed
in 736 ms; the `app` block is **not** persisted in `project.json` — export takes the manifest as a
command param, so no `PROJECT_VERSION` bump was needed; persistence is deferred to the editor).

The engine owns the cook (it holds the catalog and the shader compilers), exposed as one
`export-app` control command. The Tauri bridge re-implements no cooking.

## `app` config block

- Add an `app` block to the project document (`engine/crates/assets/src/project.rs`):
  `{ title: String, width: u32, height: u32, fullscreen: bool, vsync: bool }`, defaulted when
  absent. Bump `PROJECT_VERSION`; older `project.json` re-save to gain it (data migration out of
  scope per AGENTS.md — the version gate rejects, the user re-saves).
- Surface it in the project DTO (`engine/crates/protocol/src/dto.rs`) so the editor round-trips it
  through `get-project` / `save-project`. Regenerate `@saffron/protocol` (`xtask gen-protocol` /
  `bun run check`).

## `export-app` command

- Register in `saffron-control` (a `commands_*.rs` registration), handled in
  `engine/crates/host/src/control_renderer.rs` against the host's `AssetServer`.
- Params: `{ output_dir, app: <the app block> }`. Result: `{ path, warnings[] }`.

## Cook steps (synchronous)

1. **Pre-bake material SPIR-V.** Iterate the catalog; for each material with a node-graph call the
   existing `AssetServer::compile_material_graph` + `compile_material_mesh_shader` (+ preview if
   shipped) from `engine/crates/assets/src/codegen.rs`, so `materials/<uuid>*.spv` all exist. If
   any compile fails, **fail the export** (don't ship a broken app).
2. **Stage the folder:**
   - copy the `saffron-player` binary (sibling of the running host in the target dir → locate via
     current-exe dir),
   - copy `assets/` (cooked catalog incl. baked `.spv`),
   - copy `src/` (Luau scripts),
   - copy engine `shaders/` (the `.spv` tree next to the host, via `resolve_shader_dir`),
   - copy `project.json`,
   - write `app.json` from the `app` block.

Final layout:

```
<Title>/
  saffron-player
  project.json  app.json
  shaders/  assets/  src/
```

## Watch-outs

- **Synchronous cook freezes the host frame loop** while shaders compile — acceptable for a
  deliberate action in v1; note it for a future streamed-progress pass.
- Locating the player binary: it sits beside `saffron-host` in `target/<profile>/`; resolve from
  the running exe's directory, and surface a clear error if it's missing (a release build that
  didn't build the player).
- Whole catalog is copied — no dead-asset stripping in v1 (named in the README scope).

## Gate

`just engine` clean; control-schema contract test + `bun run check` green (new command + DTO). A
new `tests/e2e` test drives `export-app` over the control plane, asserts the staged folder is
complete, then boots `saffron-player` against it with `SAFFRON_EXIT_AFTER_FRAMES=N` and asserts a
validation-clean run. `just prepare-for-commit` clean.
