# Phase 5 — editor loading states

**Status:** COMPLETED

Editor-only, no ordering dependency on the engine phases. `AssetTile` conflates "still
fetching" with "has no thumbnail": `url === null` renders the type icon in both cases
(`editor/src/components/AssetTile.tsx:214-219`), so during a slow fetch the grid just
looks wrong rather than busy.

## The work

- Track a per-tile fetch state alongside the url — `loading` while the
  `getThumbnailUrl` promise (`AssetTile.tsx:144-159`) is outstanding, `ready` on resolve,
  `none` on reject (keep the type-icon fallback for `none`).
- Render the loading state with a shadcn-consistent affordance: a `Loader2` spin (or
  `Skeleton` shimmer) over a dimmed type icon in the existing square
  (`AssetTile.tsx:214`). Theme tokens only (`bg-muted` / `text-muted-foreground`), per the
  panel rules.
- A cache hit (`getCachedThumbnailUrl`, `AssetTile.tsx:134-136`) must never flash the
  spinner — initial state is `ready` when the cached url exists.
- Apply the same state to the other thumbnail consumers so behaviour is uniform:
  `AssetPicker` (`editor/src/components/AssetPicker.tsx:89-98`) and the asset viewer tab's
  `view-asset` image if it shares the fallback pattern.
- A rejected fetch still surfaces nothing today (`AssetTile.tsx:153-155` swallows it
  deliberately — the icon *is* the result). Keep that: a missing thumbnail is not an
  error toast. Only the visual loading distinction changes.

## Verification

- `cd editor && bun run check && bun run lint && bun run format`.
- Manual: fresh project open shows spinners that resolve tile-by-tile; assets with no
  thumbnail (unsupported types) settle to the type icon; re-opening a folder with cached
  thumbnails shows no spinner flash.
- Docs: one line on the loading state in
  `docs/content/explanations/ui-and-editor/assets-panel-and-thumbnails.md`.
