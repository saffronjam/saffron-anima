# Thumbnails efficiency plan

**Status:** COMPLETED

Thumbnail generation freezes the editor. Every `get-thumbnail` runs synchronously on the
engine main thread inside the per-frame control drain (`pollControl` in `host.cppm:834` →
`drainControlServer` in `control_server.cpp:241`), so while a thumbnail is being made no
frame is rendered or published — the viewport and the whole editor stall. The texture path
makes it far worse than it needs to be: `encodeTextureThumbnailPng`
(`renderer_thumbnail.cpp:672`) discards the requested size and reads the texture back at
its **native extent**, then PNG-encodes it full-res and base64s the result into one JSON
line. For a 4k HDR (4096×2048 RGBA16F) that is a 64 MB readback, an 8.4M-pixel
single-threaded stb PNG encode, and a multi-MB reply — to draw a 72 px tile. Three
`device.waitIdle()` calls per texture thumbnail (`renderer_textures.cpp:428`,
`renderer_thumbnail.cpp:684`, `renderer_thumbnail.cpp:722`) drain the entire GPU each time.

Nothing is cached across restarts: the engine caches only the GPU texture in-session
(`assets.cppm:1798`), and the editor's blob-URL cache (`editor/src/state/store.ts:1472`)
is wiped on every project load. A project with ~100 textures pays ~140 sequential
main-thread freezes at every startup, with the HDR as a multi-second spike that can blow
the editor bridge's 5 s read timeout (`editor/src-tauri/src/lib.rs:279`) — the editor
gives up while the engine finishes the work anyway.

The plan fixes this in dependency order: right-size the readback first (removes ~95% of
the per-thumbnail cost), persist thumbnails across restarts, give the editor a real
loading state, then take generation off the main thread entirely.

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`).
Mark a phase `COMPLETED` when its work is done and validation-clean; delete a phase file
only *after* it is `COMPLETED` and merged.

## Phases

| Phase | What | Status |
|---|---|---|
| [1 — GPU downscale before readback](phase-1-gpu-downscale-readback.md) | respect the `size` hint: blit textures down on the GPU before readback; truthful `ThumbnailResult` dimensions | COMPLETED |
| [2 — tonemapped HDR previews](phase-2-hdr-tonemapped-previews.md) | tonemap HDR asset thumbnails instead of clamping to white | COMPLETED |
| [3 — disk thumbnail cache](phase-3-disk-thumbnail-cache.md) | engine-side PNG cache keyed by asset id + source stat, survives restarts | COMPLETED |
| [4 — cache invalidation + CLI](phase-4-cache-invalidation-and-cli.md) | invalidate on delete/material edits, orphan cleanup, `se` cache command | COMPLETED |
| [5 — editor loading states](phase-5-editor-loading-states.md) | distinguish loading from no-thumbnail in `AssetTile`; spinner while fetching | COMPLETED |
| [6 — scoped GPU waits](phase-6-scoped-gpu-waits.md) | replace the thumbnail path's `device.waitIdle()` calls with per-submit fences | COMPLETED |
| [7 — async thumbnail generation](phase-7-async-thumbnail-generation.md) | worker-thread generation + pending/re-poll protocol; the frame loop never blocks | COMPLETED |

Phases 1–2 are one task (right-size the work), 3–4 one task (persistence), 5 one task
(editor UX), 6–7 one task (unblock the frame loop). 1–5 have no ordering constraints
between tasks beyond 2 after 1 and 4 after 3; 6 is groundwork for 7. Re-measure after
phases 1–4 land: if cold thumbnails are cheap enough and warm starts are disk reads,
phase 7's added threading complexity may not pay for itself.
