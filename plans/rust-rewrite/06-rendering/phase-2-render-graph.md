# Phase 2 — the render graph: `RgUsage`→barrier derivation as a standalone unit

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-1-device-swapchain-bringup

## Goal

Port the render-graph barrier-derivation engine — the silent-failure heart of the renderer. A pass
*declares* what it touches (`RgUsage` reads/writes + color/depth attachments); the graph derives every
`pipelineBarrier2`, every layout transition, and the cross-frame layout write-back. No pass ever writes
a barrier by hand. This phase is self-contained and **heavily unit-tested in isolation** before any real
pass exists, because a missing or wrong barrier is a data race, not a compile error.

## Why this shape (NO LEGACY)

- **The graph is ported literally — same `RgUsage` set, same `usageInfo` table, same `applyAccess`
  hazard logic.** This is the one place the rewrite is a faithful transcription rather than a
  re-architecture: the logic is correct, total, and load-bearing, and any "improvement" risks a silent
  desync. The `RgUsage` enum (10 variants) becomes a Rust `enum`; `usageInfo` becomes a `match`
  returning `RgUsageInfo { stage, access, layout, is_write }`; `applyAccess` keeps the exact
  hazard rule (`(is_write && touched) || (!is_write && last_was_write)`) and the image-vs-buffer split
  (images barrier on layout-change-or-hazard; buffers on hazard only).
- **`std::function<void(CommandBuffer)>` → `Box<dyn FnOnce(CommandBuffer)>`.** A pass body runs exactly
  once, on the render thread, while the command buffer records. `FnOnce` is the right bound; the closure
  captures resolved handles (not `&mut Renderer`), per README §2. Recording is single-threaded, so the
  closures are `!Send` and that is fine.
- **The cross-frame layout write-back stays a write-back, expressed safely.** The C++
  `externalLayout` is a `vk::ImageLayout*` the graph writes after execute, so an image's layout carries
  to next frame (`render_graph.cppm:99,712`). In Rust this is *not* a raw pointer: `import_image` takes
  the resource's owning layout slot by index/handle, and `execute` writes the resolved layout back
  through the borrow (or returns the per-resource final layouts the caller applies). The contract — an
  imported image's entry layout is its last-frame exit layout — is preserved exactly.
- **`RgResource` stays an index handle into the graph's resource table** (`render_graph.cppm:46`); a
  `u32` newtype. The graph owns `Vec<RgResourceState>` + `Vec<RgPass>`, rebuilt every frame (cheap).
- **The profiler scope/timestamp machinery (`GpuScope`/`CpuScope`/`RgTimestamps`) is split out, not
  inlined here.** Phase 16 owns it; this phase records passes with the timestamp/label hooks as
  *optional* no-op-when-absent parameters, matching the C++ `executeRenderGraph(graph, cmd, timestamps,
  labels, cpu)` signature where every instrument is nullable. Keeping the hooks in the signature now
  avoids reshaping `execute` later.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/render_graph.cppm` — the whole unit:
  - `RgUsage` (`:25`, the 10 variants), `RgPassKind` (`:39`), `RgResource` (`:46`), `RgAccess` (`:51`),
    `RgAttachment` (`:63`, incl. the MSAA `resolve`), `RgPass` (`:75`), `RgResourceState` (`:87`,
    incl. `externalLayout`), `RenderGraph` (`:104`).
  - `usageInfo` (`:281`) — the stage/access/layout/is_write table per usage.
  - `seedImageState` (`:325`) — a freshly-imported ShaderReadOnly image seeds its WAR source.
  - `applyAccess` (`:342`) — the hazard rule + image/buffer barrier emission + state advance.
  - `importImage`/`importImage3D`/`importBuffer` (`:392`/`:411`/`:419`), `addPass` (`:428`).
  - `executeRenderGraph` (`:541`) — derive barriers, open the rendering scope (graphics) incl. MSAA
    color `eAverage` / depth `eSampleZero` resolve, run the body, close, write back external layouts.
- README §2 (closures capture handles, not the aggregate).

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named unit tests over the pure barrier logic (no GPU needed —
  the table + hazard rule are pure functions):
  - `usage_info` returns the exact (stage, access, layout, is_write) tuple for all 10 `RgUsage` variants
    (golden table lifted from `usageInfo`).
  - `apply_access` emits an image barrier on a layout change, on a write-after-touch hazard, and on a
    read-after-write hazard — and emits *nothing* for a read after a read (no false barrier).
  - a buffer emits a memory barrier on hazard only, never on a no-op read.
  - a multi-pass sequence (skin compute-write → vertex-input read → color write) produces exactly the
    expected barrier list, in order.
  - the cross-frame layout write-back: an imported image's exit layout becomes its next-frame entry
    layout.
- A validation-clean GPU smoke: a two-pass graph (compute write → graphics sample) records and runs with
  zero validation messages, proving the derived barriers are real-GPU-correct.
