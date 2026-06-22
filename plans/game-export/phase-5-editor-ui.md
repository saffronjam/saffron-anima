# Phase 5 — editor "Export App…" UI

**Status:** COMPLETED — "Export App..." item in `ProjectMenu.tsx` (own group above Exit,
`disabled={!editing || !project}`); new `ExportModal.tsx` (Dialog: app title seeded from the
project display name, output folder via `open({directory:true})` through `withNativeDialog`,
width×height, fullscreen + vsync `Switch`es, static "Target: Linux (x86_64)" label, `busy`/`status`
+ `notify`/`notifyError`); `exportModalOpen` + setter in the Zustand store; `client.exportApp(...)`
typed wrapper; `AppManifest`/`ExportApp*` re-exported from `protocol/index.ts`; mounted in
`App.tsx`. Verified: `bun run check` (gen:protocol + tsc) clean; `bun run lint` 0 errors (the 13
warnings are pre-existing). Visual confirmation of the dialog is a user check (`just run`).

Compose existing editor primitives; no new UI framework. The modal gathers settings and shows
busy/result state; the cook runs engine-side over the `export-app` command from Phase 4.

## Menu item

- `editor/src/app/ProjectMenu.tsx`: add `Export App…` in its own group (leading
  `DropdownMenuSeparator`) above `Exit`, `disabled={!editing || !project}` — matching the existing
  item conventions (`onSelect` opens the modal via a Zustand action).

## Modal

- New `editor/src/app/ExportModal.tsx`, modeled on `editor/src/app/ProjectStartupModal.tsx`
  (`Dialog` + `busy`/`status` pattern). Fields:
  - **App title** (`Input`, defaults from project `displayName`),
  - **Output folder** (`Input` + folder pick via `open({ directory: true })` through
    `withNativeDialog` from `editor/src/state/store.ts`),
  - **Width × Height** (two numeric `Input`s),
  - **Start fullscreen** (`Switch`), **VSync** (`Switch`),
  - a static **"Target: Linux (x86_64)"** label (not a picker in v1).
- Mount it in `editor/src/app/App.tsx`; add `exportModalOpen` + setter (and any draft settings) to
  the Zustand store (`editor/src/state/store.ts`).

## Wiring

- Regenerate the protocol (`bun run gen:protocol`) so the `app` block + `export-app` types exist,
  then add `exportApp(params)` to `editor/src/control/client.ts` (the `call("export-app", …)`
  pattern).
- On submit: set `busy`, call `client.exportApp({ outputDir, app })`; on success `notify(...)` (and
  close), on failure `notifyError(...)` / inline `status` (the `editor/src/lib/flash.ts` helpers).
- Persist the entered settings back into the `app` block via `save-project` so the modal remembers
  them between exports.

## Watch-outs

- Tauri file dialogs are not window-modal under the reparented-viewport setup; use
  `withNativeDialog` (it blocks concurrent dialogs and greys the menu) — do not call `open()` raw.
- The cook is synchronous engine-side, so the call may take seconds; keep the button in a
  `busy`/"Exporting…" state and don't let the modal be dismissed mid-flight.

## Gate

`cd editor && bun run check && bun run lint` clean; manual `just run` → **Export App…** → produces a
runnable folder that launches with `saffron-player`.
