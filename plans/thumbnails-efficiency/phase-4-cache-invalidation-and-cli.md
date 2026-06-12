# Phase 4 — cache invalidation and CLI

**Status:** COMPLETED

After phase 3. The stat-stamp key self-invalidates for file edits, but three lifecycle
paths leave wrong or dead entries: deleting an asset orphans its PNGs, re-import mints a
new uuid (orphaning the old one), and editing a *parent* material reflows instances
without touching their `.smat` files (stale previews). Per the repo rule, new
drivable/inspectable engine state also gets a control command.

## The work

- **Delete:** `delete-asset` (`control_commands_asset.cpp`) removes every cache file for
  that uuid (all sizes) alongside the asset file.
- **Materials:** key material thumbnails on the *resolved* state, not the file stamp — a
  short hash of the resolved params + texture uuids after `applyOverrides`
  (`assets.cppm`), so a parent edit changes every instance's key and stale entries fall
  out naturally. Material save/update paths need no explicit purge beyond that.
- **Orphan sweep:** on project load (after `clearAssetCaches`,
  `assets.cppm` project-load path), delete cache files whose uuid is no longer in the
  catalog. Keeps the dir bounded without a background task.
- **Control command:** one `thumbnail-cache {action: "stats" | "clear"}` command in
  `control_commands_asset.cpp` — `stats` returns entry count + total bytes, `clear` empties
  the project's cache dir. Full DTO workflow: declare params/result in `control_dto.cppm`,
  add the command + fixture to `tools/gen-control-dto/gen.ts`, regenerate and commit all
  five outputs (serde, scene serde, `se-types.ts`, OpenRPC, manifest). Reachable from
  `tools/se` as `se thumbnail-cache stats|clear`.

## Verification

- e2e: delete an asset → its cache files are gone; edit a parent material via
  `material-update` → the instance's `get-thumbnail` returns a different PNG;
  `thumbnail-cache stats` counts match the dir; `thumbnail-cache clear` empties it.
- Contract test: `make schema` (the manifest-driven contract test) passes with the new
  command's fixture.
- Milestone gate: `make engine` + `make prepare-for-commit`.
- Docs: add the command to `docs/content/reference/control-commands.md` and the
  invalidation story to the assets-panel page.
