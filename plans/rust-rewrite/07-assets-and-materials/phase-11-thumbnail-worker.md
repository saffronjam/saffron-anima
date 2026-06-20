# Phase 11 — the thumbnail worker

**Status:** COMPLETED

**Depends on:** 07-assets-and-materials:phase-4-resolve-and-load-paths, 07-assets-and-materials:phase-7-render-ready-materials, 06-rendering:phase-16-capture-shm-profiler (render_mesh_thumbnail/render_material_preview/bind_thumbnail_worker_thread)

## Goal

Port the off-thread thumbnail worker: the `ThumbnailWorker` (a `JoinHandle` + an `Arc<Mutex<WorkerState>>`
+ a `Condvar`), the worker loop (wait → pop → decode → upload via rendering's thumbnail primitives →
handback), the enqueue/dedup path (`generate_thumbnail` + the sync fallback), `drain_thumbnail_completions`
(main-thread per-frame insert into the caches), and the lifecycle (`start_thumbnail_worker`,
`stop_thumbnail_worker`, `clear_thumbnail_queue`) with its teardown ordering.

## Why this shape (NO LEGACY)

This is the **only** cross-thread shared-mutable site in the crate and exactly the marked GPU-queue
sharing thread, so it is the legitimate `Arc<Mutex<WorkerState>>` (foundations Ref bucket 2). `WorkerState`
holds the job `VecDeque`, the `in_flight`/`failed` dedup `HashSet<String>` (keyed by cache path), the two
handback `Vec`s (`(Uuid, Arc<GpuTexture>)`, `(Uuid, Arc<GpuMesh>)`), and the `stop` flag — guarded by one
`Mutex`, woken by a `Condvar`, matching the C++ `std::mutex` + `std::condition_variable`. The C++
`std::thread` becomes a `std::thread::spawn` returning a `JoinHandle`; `Drop` does not join (an explicit
`stop_thumbnail_worker` joins, because join must happen *before* `wait_gpu_idle`/renderer teardown so the
worker's last submit's fences have completed). The worker **decodes on its own thread** then calls
rendering's `upload_texture`/`render_material_preview`/`render_mesh_thumbnail` (which take the queue +
bindless mutexes internally) bound to the worker's dedicated command pool via
`bind_thumbnail_worker_thread`. The handback `Arc`s cross the thread boundary (the GPU handles are
`Send`). The dedup-on-cache-path avoids re-enqueueing an in-flight or failed job. The sync fallback (no
worker) generates inline and returns the result directly; the worker path replies "pending".

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets_thumbnail.cpp`: `ThumbnailWorker` (`thread`, `mutex`, `cv`,
  `queue` deque, `inFlight`/`failed` sets, `textureHandback`/`meshHandback`, `stop`), `ThumbnailJob`,
  `ThumbnailTextureSource`, `thumbnailWorkerLoop` (the `cv.wait(stop || !queue.empty())`, decode, upload,
  `inFlight.erase`, handback push / `failed.insert`), `startThumbnailWorker` (prewarm pipelines on the
  main thread first, then spawn), `stopThumbnailWorker` (set stop → notify → join → drop, before
  `waitGpuIdle`), `clearThumbnailQueue` (abandon queued + dedup + handback, GPU idle), `drainThumbnailCompletions`
  (swap the handbacks under the lock, insert into caches), `generateThumbnail`, `thumbnailUploadTexture`.
- Upstream rendering: `render_mesh_thumbnail`, `render_material_preview`, `bind_thumbnail_worker_thread`,
  `prewarm_thumbnail_resources`, the queue/bindless mutexes (the two `Arc<Mutex>` GPU sites).
- The AGENTS rule on the worker handing finished GPU resources back to the main thread.

## Acceptance gate

- `cargo build -p saffron-assets` + workspace green; clippy + fmt clean; the crate root stays
  `#![deny(unsafe_code)]` (the threading uses only safe `std::thread`/`Mutex`/`Condvar`).
- `#[test]`s (with a stub renderer whose thumbnail render returns a counting `Arc`): enqueue a job →
  worker decodes + uploads → `drain_thumbnail_completions` inserts the `Arc` into the texture cache;
  enqueueing the same cache path twice does not double-enqueue (dedup via `in_flight`); a failing job
  marks the cache path `failed` and is not retried.
- A lifecycle `#[test]`: `stop_thumbnail_worker` joins the thread (no panic, no deadlock) and is called
  before a recorded `wait_gpu_idle`; `clear_thumbnail_queue` empties queue + dedup + handbacks; an
  already-running job's single handback is dropped on the next clear.
- A `#[test]` asserts the worker `Arc<Mutex<WorkerState>>` is the only shared-mutable site (the caches +
  catalog are main-thread `&mut`); a `Send` assertion on the handback `Arc<GpuTexture>`/`Arc<GpuMesh>`.

## Follow-on closed: the codegen preview-shader argument

`ThumbnailGpu::render_material_preview` now carries `shader_spv: Option<&Path>` (the C++
`renderMaterialPreview`'s `shaderSpv`). The off-thread worker tile passes `None` — the disk-cached
material thumbnail renders through the cached default studio preview, exactly as the C++ worker's
3-arg `renderMaterialPreview(renderer, sm, size)` does. The live `preview-render` control command is the
only caller that compiles the `_preview.spv` for a non-foldable graph (it has the `AssetServer`) and
passes it through the host seam to the renderer's per-call codegen pipeline.

## Post-integration fix (e2e exposed)

The unit gate built the worker against a stub GPU; the host never started a *real* worker, so the
host's `thumbnail_worker` stayed `None` and every `get-thumbnail`/`view-asset` took the synchronous
fallback (replied `pending: false`, blocking the frame loop on the ~1s 4K-HDR decode + upload +
render — `thumbnail_async.test.ts`). The blocker was the absence of a `Send + 'static` worker GPU
seam. Closed: `Renderer.device` / `Renderer.descriptors` are now `Arc<Device>` / `Arc<Descriptors>`
(both `Send + Sync` — ash handles + the VMA allocator + the bindless `Mutex` are thread-safe, and
every slot claim/write serializes through that mutex), shared via `device_arc()` / `descriptors_arc()`.
The new `WorkerThumbnailGpu` (in `saffron-host`) holds those `Arc`s + its own per-thread `Uploader`
(shared queue behind its `Arc<Mutex>`) + its own prewarmed `ThumbnailRenderer` (per-thread command
pools + PSO cache), implementing `ThumbnailGpu`. The host starts the worker in `on_attach`
(`start_thumbnail_worker`) after prewarming the renderer's own pipelines; the existing teardown joins
it before `wait_gpu_idle`. A cold thumbnail now enqueues + replies `pending`, the worker generates
off-thread, and the retry is a disk-cache hit — validation-clean.
