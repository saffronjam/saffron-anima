# Phase 6 — `sa export` CLI + docs

**Status:** COMPLETED — `sa export <dir> [--title --width --height --fullscreen --no-vsync]` typed
subcommand (`engine/crates/sa/src/main.rs`) builds a partial `app` manifest (omitted fields default
on the engine) and calls `export-app`; builds + `clippy -D warnings` clean. Docs how-to page
`docs/content/how-to/export-a-standalone-app.md` (editor + CLI flows, what the cook does, v1 limits,
the What|File|Symbols table) + a hub row in `how-to/_index.md`.

Make export scriptable/CI-able (Godot's headless-export parity) and document the concept, closing
out the feature per the AGENTS.md keep-current rules.

## `sa export`

- Add an `export` subcommand to the `sa` CLI (`engine/crates/sa`, clap) that connects to a running
  host's control socket and calls the same `export-app` command from Phase 4:
  `sa export <output-dir> [--title …] [--width …] [--height …] [--fullscreen] [--no-vsync]`,
  defaulting unspecified fields from the project's `app` block.
- This is the "a feature that adds drivable state gets a matching control command/CLI" rule — one
  command, reused by both the editor modal and the CLI.

## Docs

- One page under `docs/content/` (Diátaxis **how-to**, e.g. "Exporting an app"): lead with the
  concept (template-runtime + data, what's in the staged folder, what's stripped vs the editor),
  then the editor flow and the `sa export` flow. Slim `What | File | Symbols` pointer table
  (`saffron-player`, `export-app`, the `app` block). Add the hub row in the matching `_index.md`.
- Use the `docs-page` skill conventions (title = body H1, plain voice, run prose through
  `humanizer`).

## Gate

Docs build clean (`hugo` / `just run-docs` preview, no broken links); `just prepare-for-commit`
clean across the workspace + editor. This is the final milestone gate for the feature — mark the
`plans/game-export/README.md` and each phase `**Status:** COMPLETED`, then the plan files may be
removed once everything is verified green.
