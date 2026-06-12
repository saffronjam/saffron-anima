# Phase 7 — async thumbnail generation

**Status:** NOT STARTED

After phase 6. The structural fix: even a cheap thumbnail still runs inside
`drainControlServer` (`control_server.cpp:241-317`) on the main thread, ahead of the
animation tick and the frame (`host.cppm:825-845`), and the control protocol is strict
synchronous request/reply — the reply is written before the drain moves on. With phases
1–4 landed, the remaining per-miss cost is source decode + upload + a small render; this
phase moves that off the frame loop so a cold-cache startup never hitches at all.

**Gate before starting:** re-measure after phases 1–4. If cold thumbnails are <~10 ms and
warm starts are pure disk reads, the threading complexity here may not be worth it — that
is a legitimate outcome; record it in this file and close the plan.

## The work

- **Worker:** one engine thumbnail worker thread owning its own `vk::CommandPool` (the
  current code borrows `renderer.frame.frames[0].commandPool`,
  `renderer_thumbnail.cpp:695`, which is main-thread-only). Queue submission is externally
  synchronized in Vulkan — guard `graphicsQueue.submit2` with a mutex shared with the
  frame loop's submit sites (or use a second queue from the family where available; the
  mutex is the portable baseline). Waits are the phase 6 per-submit fences.
- **CPU-side loading too:** the worker also runs the source decode
  (`decodeImageHdr`/`decodeImage`) and `uploadTexture*` for thumbnail-triggered loads —
  that is where the 4k HDR's remaining seconds live. The `AssetServer` GPU caches
  (`assets.cppm:51-57`, `textureRefByUuid`) are main-thread state today; either hand the
  finished `Ref` back to the main thread for cache insertion (worker → completion queue →
  inserted during `pollControl`), or mutex the maps — prefer the handback, it keeps the
  caches single-threaded.
- **Protocol:** `get-thumbnail`/`view-asset` on a cache miss enqueue a job and reply
  immediately with a pending marker (an optional `pending: true` on `ThumbnailResult`, or
  a small status DTO — decide in `control_dto.cppm`, regenerate the five outputs). The
  editor re-requests on pending with backoff; a completed job's PNG lands in the phase 3
  disk cache, so the retry is a pure cache hit. This keeps the wire request/reply (no
  server push) and the `CONTROL_IO` serialization (`editor/src-tauri/src/lib.rs:265`)
  healthy: pending replies return in microseconds, so the reconcile poll never queues
  behind generation.
- **Editor:** `getThumbnailUrl` (`editor/src/state/store.ts:1509-1534`) handles the
  pending reply by scheduling the retry and keeping the phase 5 `loading` state; the
  in-flight dedup map already prevents request storms.
- **Teardown:** the worker drains and joins before `waitGpuIdle`/renderer destroy (the
  `run` teardown contract in the root `AGENTS.md`); in-flight jobs either complete or are
  abandoned before device teardown begins.
- Optional, only if profiling still shows it: vectorize the scalar `floatToHalf` loop
  (`renderer_textures.cpp:346-348`, 33.5M conversions for a 4k HDR) — off the main thread
  it no longer stalls frames, so this is polish, not correctness.

## Verification

- e2e: cold-cache `get-thumbnail` returns pending then resolves on retry; frame-time
  assertion — drive frames while a 4k HDR thumbnail generates and assert no frame gap
  above a threshold (the suite boots headless engines and reads stats over the control
  plane).
- Contract test (`make schema`) for the DTO change; editor `bun run check`.
- Validation-clean run with thumbnails generating during heavy scene render.
- Milestone gate: `make engine` + `make prepare-for-commit`.
- Docs: update the generation story in
  `docs/content/explanations/ui-and-editor/assets-panel-and-thumbnails.md` and
  `mesh-thumbnails.md` (no longer "rendered synchronously on request").
