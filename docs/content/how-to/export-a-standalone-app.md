+++
title = 'Export a standalone app'
weight = 11
math = false
+++

# Export a standalone app

Turn the open project into a folder you can hand to someone who does not have the editor: a
`saffron-player` runtime binary plus your cooked project data. It is the *template + data* split
Godot and Unreal use — a precompiled runtime beside packed content — so the shipped app needs no
editor, no control plane, and no shader toolchain (`slangc`). v1 targets Linux (x86_64) and stages
loose files (no single-file packing yet).

## What you get

```text
MyApp/
  saffron-player          the runtime binary
  app.json  project.json  the manifest + the cooked project
  assets/  src/  shaders/  cooked assets (incl. pre-baked material SPIR-V), Luau scripts, engine shaders
```

Run it with `./MyApp/saffron-player`: it loads the `project.json` beside itself, opens a window, and
runs the scene live — animation, physics, and scripts — through the same runtime the editor's play
mode uses.

## In the editor

1. Save the project — export stages the saved `project.json`.
2. Open the project menu (top-left) ▸ **Export App…**.
3. Set the **App title**, pick an **Output folder**, set the window **Width × Height**, and toggle
   **Start fullscreen** / **VSync**.
4. Click **Export**. The engine pre-bakes the materials, copies the player + data, and writes
   `app.json`. A toast reports the staged path (and any non-fatal warnings).

## From the CLI

The same cook is scriptable against a running engine over the [`sa` CLI](../drive-the-editor-from-the-cli/):

```sh
sa export ~/MyApp --title "My App" --width 1280 --height 720
sa export ~/MyApp --fullscreen --no-vsync   # omitted flags fall back to defaults
```

`sa export <dir>` invokes the same `export-app` command the editor's dialog does.

## What the cook does

- **Pre-bakes material shaders.** Each material's node graph compiles to SPIR-V *now*, into the
  staged `assets/`, so the player loads `.spv` and never runs `slangc`. (See
  [node-graph codegen](../../explanations/materials-and-pipelines/node-graph-codegen/).)
- **Stages the runtime + data.** The `saffron-player` binary and the engine `shaders/` (both built
  beside the host), plus the project's `project.json`, `assets/`, and `src/`.
- **Writes `app.json`** from your settings — the manifest the player reads at startup.

## v1 limits

Linux x86_64 only; loose-folder staging (no single-file pack); the whole asset catalog is copied
(no dead-asset stripping); and `fullscreen` / `vsync` are recorded in `app.json` but not yet applied
by the player's window backend (it presents windowed, FIFO/vsync-on).

| What | File | Symbols |
|---|---|---|
| The export cook + staging | `engine/crates/control/src/commands_asset.rs` | `export-app` handler · `export_app` |
| The standalone runtime | `engine/crates/player/src/main.rs` | `saffron-player` · `PlayerLayer` |
| The shared play spine the player runs | `engine/crates/runtime/src/session.rs` | `RuntimeSession` |
| The app manifest | `engine/crates/protocol/src/dto.rs` | `AppManifest` · `ExportAppParams` |
