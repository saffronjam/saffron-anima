# dmabuf viewport plan

This plan upgrades the editor's viewport transport from wl_shm to linux-dmabuf: the engine
exports its publish images as GPU dma-buf fds, the editor wraps them as `zwp_linux_dmabuf_v1`
wl_buffers, and the compositor samples engine-rendered memory directly — zero-copy, with a
release-driven buffer lifecycle replacing today's race-prone ring overwrite. The shm path
(see the [viewport compositing](../../docs/content/explanations/ui-and-editor/viewport-compositing.md)
explanation) works and presents at monitor refresh; this plan removes its two structural
defects: the compositor's per-frame CPU→GPU upload (~1.3 GB/s at 1600×900@240) and the
absence of a `wl_buffer.release` handshake (the engine can overwrite a slot mid-read).

Phase 1 recorded the measurement that scoped this plan: presentation feedback proved the
subsurface presents at the monitor rate, the felt-60 was the webview input path (fixed by
engine-side drag smoothing + fly-input), and `presented` cannot certify buffer-to-glass —
so zero-copy is pursued for correctness and efficiency, not smoothness.

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`).
Mark a phase `COMPLETED` when its work is done and validation-clean; delete a phase file only
*after* it is `COMPLETED` and merged.

## Phases

| Phase | What | Status |
|---|---|---|
| [1 — instrument wp_presentation](phase-1-instrument-wp-presentation.md) | presented/discarded counters + vblank deltas on the presenter | COMPLETED |
| [2 — linux-dmabuf buffers](phase-2-linux-dmabuf-buffers.md) | exportable Vulkan images, fd transport, dmabuf wl_buffers, release lifecycle, explicit sync | NOT STARTED |
| [3 — pacing polish](phase-3-pacing-polish.md) | fifo-v1/commit-timing-v1 cadence, native-rate pointer input | NOT STARTED |
