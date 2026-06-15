# Phase 6 — Editor (Tauri / React / Rust)

**Status:** COMPLETED

The retained-Saffron model keeps almost all editor-internal "saffron" tokens. This phase changes only
the **user-visible product name**, the bundle id (it contains "engine"), and the `sa-types` import.
The binary path, `SAFFRON_ANIMA_BIN`, and the profiler `pid` were already handled in phase 1; the
`sa-types.ts` file + `index.ts` import were handled in phase 2 — re-confirm them here.

## User-visible app identity

The product the user launches is **Saffron Anima**. Rename the editor's visible name from
"Saffron Editor" → "Saffron Anima":
- `editor/src-tauri/tauri.conf.json`: `"productName": "Saffron Editor"` → `"Saffron Anima"`;
  window `"title": "Saffron Editor"` → `"Saffron Anima"`; `"identifier": "dev.saffron.engine.editor"`
  → `"dev.saffron.anima.editor"` (contains "engine").
- `editor/index.html`: `<title>Saffron Editor</title>` → `<title>Saffron Anima</title>`.
- `editor/src-tauri/src/lib.rs`: the runtime `window.set_title("Saffron Editor")` → `"Saffron Anima"`;
  the `.expect("failed to build Saffron editor")` message → `"Saffron Anima"`.
- `editor/src-tauri/Cargo.toml`: `description = "SaffronAnima TypeScript editor shell"` →
  `"Saffron Anima TypeScript editor shell"` (description prose only).

> Decision made here (flag for review): the editor window is titled **"Saffron Anima"**, not
> "Anima Editor" — it is the face of the product. Easy to flip if you'd rather keep a distinct
> "… Editor" suffix.

## Keep unchanged (retained Saffron family)

- Crate/package names `saffron-editor`, `saffron_editor_lib` (no "engine"); `package.json` `name`.
- `SAFFRON_*` env reads other than `SAFFRON_ANIMA_BIN` (e.g. `SAFFRON_CONTROL_SOCK`,
  `SAFFRON_VIEWPORT_SHM_*`, `SAFFRON_WEBVIEW_HW`, `SAFFRON_EDITOR_NATIVE_VIEWPORT`, `VITE_SAFFRON_DEV_MODE`).
- Internal tokens: MIME `application/x-saffron-entity`; React-Flow node type `"saffron"`,
  `SaffronNode`/`SaffronNodeData` (`MaterialGraphEditor.tsx`, `materials/graph.ts`); `localStorage`
  `saffron.*` keys (`state/store.ts`); sockets `saffron-editor-*.sock`; shm `/saffron-viewport-*`;
  `c"saffron-backdrop"` (`wayland_viewport.rs`); the `[saffron]` log prefix; `saffron-profile.json`;
  the `"Saffron Project"` file-dialog label (`ProjectStartupModal.tsx`, `ProjectMenu.tsx`).

## Re-confirm (already changed earlier)

- `editor/src/protocol/index.ts` imports `./sa-types` (phase 2); no `sa-types.ts` remains.
- `editor/src/lib/chromeTrace.ts` pid is `"SaffronAnima"` and `profilerTransforms.test.ts` matches (phase 1).
- The engine default path is `build/debug/bin/SaffronAnima` and the env var is `SAFFRON_ANIMA_BIN` (phase 1).
- Editor-only asset drag/drop MIME channels use `application/x-sa-asset` / `application/x-sa-folder`;
  the shared constants keep panel drop targets and tests on the same names.

## Verify

```sh
cd editor && bun install && bun run check && bun run build
```
`bun run check` regenerates the protocol and typechecks clean; the build succeeds. The Tauri window
title reads "Saffron Anima". `bun run lint` (oxlint) and `bun run format` (oxfmt) are clean.
