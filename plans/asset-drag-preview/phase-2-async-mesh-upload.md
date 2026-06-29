# Async mesh upload: tri-state resolve, off-thread BLAS, debounced spinner

**Status:** NOT STARTED — deferred: needs GPU + runtime/concurrency validation

> **Why deferred (not implemented blind).** This phase is a pure runtime/concurrency change — a
> worker thread, async upload timing, and hot-draw-path skipping — with essentially no
> deterministic unit-test surface; its failure modes (deadlock, a mesh that never resolves, a
> spinner that never clears, a queue race) compile clean and only surface at runtime. The dev
> environment used for Phases 1 and 3 has **no GPU and cannot run the host** (the standalone
> headless loop does not pump here, confirmed against an unmodified HEAD build), so this phase
> cannot be validated here. Implementing it blind would ship unverified threaded Vulkan into the
> renderer's hot path, against the project bar. Phases 1 and 3 already remove the per-move
> re-instantiation and the per-move O(triangles) pick; this phase removes the remaining
> *first-draw* upload/BLAS stall and is the right next step **on a machine with a GPU**.
>
> **Proven pattern to reuse — do not invent threaded Vulkan.** `ThumbnailWorker`
> (`assets/src/thumbnail.rs`) already uploads meshes **off-thread**: a `std::thread` with
> `gpu.bind_worker_thread()`, a `Mutex`+`Condvar` job queue, and `Arc<GpuMesh>` handed back to the
> main thread to drop into the cache. The renderer takes the queue + bindless mutexes internally,
> so cross-thread `upload_mesh` is already safe. The async draw-path upload should mirror this
> worker exactly (spawn, enqueue on first resolve, drain finished `Arc<GpuMesh>` into
> `mesh_by_uuid`, re-arm the redraw seam on completion) rather than building a new submission path.
> Note also that placement *bounds*/*pick* need only CPU mesh data (`load_mesh_cpu_asset` exists),
> so they need not wait on the GPU upload at all — only the ghost's *rendering* does.
>
> **Critical queue-ownership finding (read before implementing).** A deep read of the submit paths
> (done while scoping this) shows the queue-synchronization model is **not** a single shared mutex:
> - `GpuQueue` is `Arc<Mutex<vk::Queue>>` (`rendering/src/upload.rs`), but **`GpuQueue::new(vk::Queue)`
>   creates a fresh `Mutex` each call** — and the host calls it separately for the one-off `Uploader`
>   (`ensure_uploader`) and for the thumbnail worker (`start_thumbnail_worker`). Two `GpuQueue`s over
>   the *same* `VkQueue` do **not** mutually exclude.
> - The renderer's per-frame/init submits call `raw.queue_submit2(device.graphics_queue, …)`
>   **directly** (e.g. `view_target.rs`, `skinning.rs`, `lighting.rs`, `ddgi.rs`), not through any
>   `GpuQueue`. (The `GpuQueue::new` + raw submits inside `scene_pass.rs`/`rt.rs`/`instancing.rs` are
>   `#[cfg(test)]` helpers.)
>
> So adding a **second background submitter** (a mesh-upload worker alongside the thumbnail worker)
> is **not provably memory-safe by static reading** — concurrent `vkQueueSubmit` on one `VkQueue`
> from two threads is UB unless externally synchronized, and the current mutexes don't span all
> submitters. The correct fix is to make **one** `Arc<GpuQueue>` the sole submit gate (frame loop +
> every worker lock it), which is a renderer-wide change touching every submit site — and must be
> GPU-validated.
>
> **Therefore the recommended async mechanism is single-threaded fence-polled, not a worker:**
> submit the upload on the main thread *without* waiting and poll the fence on later frames — no new
> submitter, no queue-ownership change, memory-safe by construction (same thread as the frame loop).
> The cost: `upload_mesh` currently does **2–3 sequential `submit_and_wait`s** (staging copy →
> `build_mesh_blas` → optional morph buffers, `rendering/src/upload.rs:374,422,438`). Fence-polling
> means folding the copy + BLAS build into **one** command buffer with an explicit copy→BLAS-read
> barrier (today the copy's `submit_and_wait` finishing is the implicit barrier), one submit, one
> fence, and a `try_finish(handle) -> Option<Arc<GpuMesh>>` that frees staging/scratch when the fence
> signals. That barrier ordering is only verifiable on a GPU (a wrong barrier yields a silently
> corrupt BLAS, not a crash). **This is why Phase 2 is GPU-gated and was not landed in a GPU-less
> environment** — both viable mechanisms hinge on correctness that compiles clean but only a GPU run
> can confirm.
**Scope:** Engine-wide (`saffron-assets`, `saffron-rendering`) + editor (`editor/`). Benefits the
exported game, not only the preview.
**Depends on:** Phase 1 (the ghost is the thing that renders invisibly while its mesh uploads).

## Goal

The first time a mesh is drawn, its GPU resolve is a cache miss that runs a vertex upload + BLAS build
and **blocks the loop thread** on `submit_and_wait` (`engine/crates/rendering/src/upload.rs:251`). For a
high-tri asset this is the size-correlated freeze when you drag it in. Move that work off the loop
thread: the draw path skips a not-yet-ready mesh, the redraw seam re-arms when the upload lands, and the
editor shows a spinner only if the upload outlives a 100 ms debounce.

This is general rendering performance — *every* first-time mesh draw stalls today, in the editor and in
`saffron-player`.

## Design

### Tri-state resolve

`load_mesh_asset` (`assets/src/load.rs:218`) returns `Option<Arc<GpuMesh>>` today (ready / failed via
negative cache). Make it tri-state:

```rust
enum MeshLoad { Ready(Arc<GpuMesh>), Pending, Failed }
```

- **Ready** — cache hit (`mesh_by_uuid`), unchanged.
- **Pending** — first request kicks off a background upload and returns `Pending`; subsequent requests
  while in flight also return `Pending` (no duplicate uploads — track in-flight ids).
- **Failed** — negative-cached as today.

The draw gather (`gather_static_draw_list`, `render_scene.rs:879`) **skips** a `Pending` mesh — the
ghost simply does not paint until its geometry lands. Pick/bounds (`render_scene.rs:1085`,
`scene_render_aabb`) treat `Pending` as "no bounds yet" (placement falls back to the ground-plane ray).

### Off-thread upload + BLAS

The uploader today is a one-off command pool on the **shared graphics queue**
(`RendererUploader`, `assets/src/gpu.rs:79`; the synchronous `submit_and_wait`, `upload.rs:251`). Move
the upload to a worker:

- A background thread (or small pool) owns a transfer/compute submission and performs
  `upload_mesh` + `record_mesh_blas_build`, then publishes the finished `Arc<GpuMesh>` back to the
  `AssetServer` cache via a completion channel drained on the loop thread.
- **Vulkan constraints — the careful part.** Queue submission and the VMA allocator are not free to
  touch from any thread: either use a dedicated transfer queue (and a queue-family-ownership transfer /
  acquire barrier on first use by the graphics queue), or serialize submissions behind a mutex. The
  BLAS build needs accel-structure queue support. Resolve this explicitly; it is the hard scope of this
  phase and must be concurrency-validated, not screenshot-validated.
- On completion, signal the redraw seam (bump the change-gen / `request_redraw`, `layer.rs:863`) so the
  newly-ready mesh paints on the next iteration even if the cursor is still.

A pragmatic first cut if a full async queue is too large: keep the build on the worker but draw nothing
for the mesh until ready (the spinner covers the gap), and revisit a dedicated transfer queue if the
worker submission contends with the frame.

### `uploading` flag on the wire

`AssetPlacementResult` (`engine/crates/protocol/src/dto.rs:1397`) gains:

```rust
#[serde(skip_serializing_if = "std::ops::Not::not")]
pub uploading: bool,
```

`preview_asset_placement` sets `uploading: true` while the ghost's mesh(es) resolve `Pending`, `false`
once `Ready`. Regenerate `@saffron/protocol` (`cargo run -p xtask -- gen-protocol` /
`editor bun run check`).

### Debounced spinner (editor)

In `ViewportPanel.tsx` (the drag-over handler that calls `previewAssetPlacement`, `client.ts:565`): when
a reply first reports `uploading: true`, start a 100 ms timer; if a `uploading: false` reply arrives
first, cancel it; otherwise show a cursor-anchored circular spinner (a React element over the
transparent webview, positioned at the last drag coordinates) until a `false` reply clears it. The
debounce means simple models — uploaded well under 100 ms — never flash a spinner. The ghost keeps
tracking the cursor (invisible) while uploading, so it pops into place the instant the mesh lands.

## Files

| What | File | Symbols |
|------|------|---------|
| Tri-state resolve | `engine/crates/assets/src/load.rs` | `load_mesh_asset`, `mesh_by_uuid`, new in-flight set |
| Draw skip on pending | `engine/crates/assets/src/render_scene.rs` | `gather_static_draw_list`, `gather_skinned_draw_list` |
| Worker upload | `engine/crates/rendering/src/upload.rs` | `upload_mesh`, `submit_and_wait`, BLAS build; completion channel |
| Uploader seam | `engine/crates/assets/src/gpu.rs` | `GpuUploader`, `RendererUploader` |
| Redraw on completion | `engine/crates/host/src/layer.rs` | redraw re-arm on async-load done |
| Wire flag | `engine/crates/protocol/src/dto.rs` | `AssetPlacementResult.uploading` |
| Handler | `engine/crates/control/src/commands_asset.rs` | `preview_asset_placement` |
| Spinner | `editor/src/panels/ViewportPanel.tsx` | drag-over handler, 100 ms debounce timer |

## Verification

- `just engine` + `just prepare-for-commit`; `editor bun run check` after the protocol regen.
- e2e: a preview of a fresh (un-cached) asset reports `uploading: true` then a later reply reports
  `false`; the loop log stays validation-clean (no queue/sync errors) across the upload.
- Concurrency: stress repeated drag-in of several large assets in quick succession (overlapping
  in-flight uploads) under the validation layers — no double-upload, no use-after-free, no queue race.
- Manual on the RTX 3070 Ti: dragging a high-tri asset no longer freezes the viewport; a spinner appears
  for the genuinely slow ones and never for small props.
