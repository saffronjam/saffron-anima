# Frame-budget controllers + render-graph hygiene

**Status:** CORE COMPLETE (auto-quality budget controller + dynamic resolution); pass-culling + async-PSO deferred
**Scope:** Both / Game (hitting frame budget on weak hardware ships in `saffron-player`)
**Depends on:** Phase 3 (tier targets feed the auto-stepping variant)

> **Done — the frame-budget controller (auto-quality tier stepping).** `rendering/src/budget.rs`'s
> `BudgetController` turns the per-frame work-time measurement (busy + GPU-fence wait, the signal the
> engine already records) into a tier step: a sustained over-budget run steps the tier down, a
> sustained-headroom run steps up, a ≥2×-budget hitch panics down at once, with consecutive-frame
> hysteresis + a post-switch cooldown so it never oscillates; it never auto-picks `Ultra` or drops
> below `Low`. It runs in `Renderer::finalize_frame_telemetry` only when `PerfConfig::auto_quality`
> is on (a `set-perf-config` field, off by default), and its only actuator is `set_render_quality`
> (Phase 3) — no new render path. Unit-tested (5 cases in `budget.rs`); **live-validated**: with
> `auto_quality=true` + a 4 ms budget on llvmpipe (work ≫ budget) the tier auto-stepped `high → low`.
> Exposed over `sa set-perf-config --autoQuality true`. Build + clippy clean.
>
> **Done — dynamic resolution.** A per-view `render_scale` (`(0, 1]`) sizes the render targets to
> `round(desired * scale)` while the published frame stays native — the offscreen→shm and
> offscreen→swapchain blits upscale (the windowed `record_present_blit` already took separate
> src/dst extents; `record_shm_copy` now sizes its capture to the native `published_extent` and the
> blit scales, filtered `LINEAR`). The render targets reallocate at a safe frame boundary
> (`render_scene_offscreen`'s top), never mid-frame. The `BudgetController` generalized to a two-dial
> ladder: it spends tier steps first (cheaper GI is less visible than fewer pixels) and only drops
> resolution **below the `Low` floor**, restoring resolution before raising the tier on the way back
> up — a controller `Scale` step is deferred to `pending_render_scale` (a resize can't run from the
> post-submit telemetry hook). Manual override via `set-perf-config --renderScale`; reported in
> `render-stats` (`renderScale`). Unit-tested (the below-floor scale steps in `budget.rs`) and
> **GPU-validated headless on the RTX 3070 Ti**: at scale 0.5 the scene pass dropped 0.66→0.18 ms and
> the whole frame 1.9→0.77 ms, the offscreen rendered 800×450 while the publish stayed 1600×900, the
> image was correct, reverting to 1.0 was clean, and the log was validation-clean.
>
> **Deferred (documented), with rationale — the remaining deep parts:**
> - **Render-graph pass culling** — **attempted and reverted; the deferral's original warning is
>   confirmed.** The idea was to use the `external_layout` slot as the "this leaves the graph"
>   never-cull root and backward-walk reachability. It was implemented, and a single full-frame GPU
>   capture looked clean — but the `saffron-rendering` GPU validation tests caught it: **not every
>   externally-consumed resource carries an `external_layout` slot.** Some targets are imported with
>   `None` yet read *after* the graph (the offscreen sampled by `record_shm_copy`, depth in the
>   prepass tests), so reachability from external-slotted resources alone wrongly culled their
>   producers → missing layout transitions → validation errors. A *safe* cull needs an explicit
>   per-resource "external output" marking at every `import_image` site that is consumed outside the
>   graph (exactly what the original deferral said), and even then it culls **zero** today because the
>   graph has no transient resources — every resource is imported and potentially read downstream. So
>   the value is nil until transient/aliased graph resources exist; revisit it *with* them. Reverted to
>   keep the renderer correct. (Lesson: a single capture is not sufficient validation for a
>   graph-execution change — the validation-test suite is.)
> - **Async PSO compilation** — threading the PSO cache off the present path risks races; a focused,
>   separately-validated change whose absence-of-races can't be shown by a screenshot. Deferred.

## Goal

Use the per-pass GPU timings Anima already measures to *automatically* hold a frame budget, and tidy two
render-graph paths that waste work: passes that produce nothing consumed this frame, and pipeline
compiles that hitch the present loop. These are general engine wins — they matter most in the exported
game on weaker target hardware, where a fixed quality tier may still miss the frame budget.

## Design

### Dynamic resolution / auto-tier controller

The profiler already measures SSGI/TAA/GTAO/shadow pass times (`control/src/commands_render.rs`
`profiler.set-mode`; `frame_history.rs`), used only for HUD grading today. Feed them into a controller
with two modes (the measurement infra exists; only the controller is missing):

- **Dynamic resolution** — scale the offscreen render extent toward `target_fps` from measured GPU time
  vs `budget_ms`, with a UE-style **panic** immediate downscale after N consecutive over-budget frames
  and a history reset to avoid oscillation.
- **Auto tier-stepping** — step the Phase-3 `RenderQuality` tier down/up to hold the budget (coarser,
  but no resolution blur).

Precedent: UE Dynamic Resolution (scales primary screen percentage from measured GPU time vs budget;
`r.DynamicRes.MaxConsecutiveOverbudgetGPUFrameCount` panic downscale + history reset; self-contained).
The offscreen extent lives in the rendering crate's target sizing
(`rendering/src/view_target.rs`); the controller reads `frame_history` and writes the extent / tier.

### Render-graph pass culling

Anima's render graph already declares per-pass resource usage (`ColorWrite` / `SampledRead` /
`StorageImageRwCompute`, …) and derives barriers, so the dependency information needed to cull is
present. Add backward reachability from the present target and drop passes whose outputs are never
consumed this frame, with a `NeverCull` opt-out pin for side-effecting passes (e.g. readbacks, BLAS
builds). This lets tier-disabled GI/effect passes (Phase 3) fall out cleanly instead of guarding each
call site. Precedent: UE RDG `r.RDG.CullPasses` / `ERDGPassFlags::NeverCull`; Unity URP auto-removes
unconsumed passes (the real mechanism is declaring fewer pass inputs, *not* a `ClearUnusedGraphResources`
API — that name is fictional, do not cite it).

### Async PSO / pipeline compilation off the critical path

Ensure first-use shader/PSO compiles in the übershader/PSO cache (`rendering/src/pipelines.rs`) do not
hitch the present loop: compile asynchronously and render with a fallback until ready. Lower leverage for
the idle-GPU problem, but it removes a distinct class of frame spikes (first material seen, first time a
feature path is hit) that the exported game hits on level load. Precedent: Bevy `PipelineCache` compiles
on the async compute task pool by default.

## Done when

- An over-budget frame (forced by a heavy scene or a low `target_fps`) triggers dynamic-res downscale or
  tier-step that brings the frame back under `budget_ms`; recovery scales back up when headroom returns.
- Tier-disabled passes are culled from the graph (verify via `pass-timings` — the disabled pass is
  absent, not zero-cost-present); `NeverCull` passes always run.
- First-use PSO compiles no longer produce a frame spike (no stutter alarm on first material draw).
- `just engine` + `just prepare-for-commit` clean; e2e budget-controller test green; docs render-graph /
  performance page updated to describe culling + dynamic resolution.

## Note

Transient resource aliasing + async compute are **out of scope** for this plan (see the README "Out of
scope" section) — they are a VRAM optimization that adds barriers and does not address the GPU-time/heat
problem. Revisit only when VRAM pressure becomes the real constraint.
