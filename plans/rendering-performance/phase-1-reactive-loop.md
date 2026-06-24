# Reactive loop: real pacer, dirty/needs-redraw seam, named override stack

**Status:** COMPLETED
**Scope:** Editor (the wired FPS cap also benefits Game)
**Depends on:** — (nothing)

> **Done.** `saffron-app` gained a `RedrawController` (continuous flag + one-shot dirty + keep-warm
> window, default continuous so layer-less apps and the test host are unchanged) on `App`, read each
> iteration by `step_frame`; `pace_iteration` paces a rendered frame to `FrameHost::pace_target_fps`
> (the renderer's `PerfConfig.target_fps`) and an idle iteration to an 8 ms poll. `SAFFRON_MAX_FPS`,
> `max_fps_from_env`, `pace_loop`, and `LoopLimits.max_fps` are deleted, as is the editor's
> `SAFFRON_MAX_FPS=500` launch arg. The control plane reports mutation: `ControlContext::poll`
> returns `true` when a non-`is_read_only_command` (the get-/list- prefixes + the explicit
> reconcile/stats query set) ran `ok`; the host's `on_update` sets `set_continuous` from
> `render_activity_reasons` (play / smoothing / camera / animation) and `request_redraw` on a
> mutation, with a startup seed in `on_attach` so the bootstrap scene paints + converges before
> idling. Live-validated on llvmpipe: static scene → host CPU 0–2%, a mutating command → ~1471%
> burst then keep-warm then idle, read-only `render-stats` polling → stays at 0%.
>
> **Deferred to Phase 5 (observability):** the e2e idle/re-arm assertion needs a control-plane
> readout of the redraw state, which Phase 5 builds; the controller logic is covered by the
> `redraw_controller_renders_while_active_then_idles_past_keep_warm` unit test here. The override
> stack is the simple flag form for now; Phase 5 adds the named push/pop reasons surfaced over the
> CLI (the `set_reasons` plumbing is already in place).

## Goal

Stop the editor host from rendering a static scene at GPU-max speed. Replace the unconditional
render-every-iteration loop with a **reactive** one: render at the configured target rate while
something is changing, drop to a low idle heartbeat when nothing is, and re-present the last
shared-memory frame for ~0 GPU when truly idle. This is the single root-cause fix for the 100% GPU /
281 W / 232 fps symptom.

This phase establishes the **one invalidation signal** — a scene change-generation counter plus a
named override stack — that Phases 4 (converge-then-stop) and 5 (throttle + observability) consume.
Build it correctly here or those phases have nothing to gate on.

## Design

### One scene change-generation counter

A monotonically increasing `u64` is the single dirty source. It is bumped by every input that can
change the rendered image:

- any `saffron-sceneedit` mutation (transform, add/remove entity, component edit),
- any control-plane command that mutates scene/material/animation/light state (`control/src/context.rs`
  command dispatch — bump on the mutating arms),
- camera transform change (orbit/pan/zoom from the editor),
- window/viewport resize,
- async asset-load completion (a model/texture finished streaming in),
- while an animation player, physics sim, or morph weight is actively advancing (these bump every frame
  on purpose — a playing clip *should* keep rendering).

The counter lives where the loop can read it cheaply each iteration (an `Arc<AtomicU64>` shared between
the control thread, sceneedit, and the app loop). The loop snapshots it at the top of each iteration
and compares to the last-rendered value.

### Named override stack (the principled `needs_redraw`)

Rather than a single bool, a small push/pop stack of named reasons keeps the loop rendering
continuously while any reason is active (precedent: UE `SetViewportsRealtimeOverride(systemName)`).
Named entries make it debuggable — Phase 5 surfaces *which* reason is keeping the GPU hot. Reasons:
`animation-playback`, `physics-sim`, `gizmo-drag`, `thumbnail-render`, `gi-converging`, `async-load`,
`keep-warm`. The loop renders continuously while the stack is non-empty; when it empties and the
change-gen counter is stable, the loop idles.

### The reactive pacer

`pace_loop(max_fps, ...)` (`app/src/lib.rs:pace_loop`) and `max_fps_from_env`
(`app/src/lib.rs:max_fps_from_env`) are **deleted**. The pacer instead reads `target_fps` from the
renderer's `PerfConfig` (`rendering/src/frame_history.rs:PerfConfig`), exposed up to the loop through a
new `FrameHost` method (e.g. `pace_target_fps(&self) -> Option<f64>` returning
`Some(perf_config.target_fps)` on `Renderer`, `None` on the test `NoopFrameHost`). Pacing state:

- **Interacting** (override stack non-empty or change-gen just moved): pace to `target_fps` (the real
  144 / 60 / monitor rate the user set).
- **Idle but animating** (a playback override active): steady low tick (~30 fps) is acceptable for the
  editor; the play edge can request a higher tier/rate via the override.
- **Fully idle** (stack empty, change-gen stable, all temporal effects converged per Phase 4): park on
  an event-wait with a slow non-zero heartbeat. **Never hard-stop** (GPU down-clock wake-stutter, locked
  decision in the README). Keep a ~0.5–1 s keep-warm window at full rate after the last interaction
  (pushed as the `keep-warm` override) so the first post-interaction frame is not janky.

In the headless `drive` loop (`app/src/lib.rs:drive`), the bare `while app.running { step_frame }`
gains the same gate: snapshot change-gen + override stack; if clean and converged, skip
`step_frame`'s render path and re-present the last published shared-memory frame (the editor already
presents from shmem, so re-presenting is ~0 GPU on the host), then sleep the heartbeat interval. In the
windowed `about_to_wait` (`app/src/lib.rs:about_to_wait`), replace `ControlFlow::Poll` +
unconditional `window.request_redraw()` with `ControlFlow::WaitUntil(next_heartbeat)` and a
`request_redraw()` issued only when dirty/animating/converging.

### Editor launch arg

`editor/src-tauri/src/lib.rs` stops setting `SAFFRON_MAX_FPS="500"`. The host now derives its cap from
`PerfConfig.target_fps`, which the editor already drives over `set-perf-config`. Delete the env branch
entirely (NO LEGACY) — there is no second pacing input.

### Control surface

`set-perf-config` / `get-perf-config` (`control/src/commands_render.rs`) already carry `target_fps`;
no new field needed for the cap. Add a `viewport.invalidate` command (force one redraw / bump change-gen)
and expose the current redraw state for tests and Phase 5 — see Phase 5 for the full readout. The e2e
suite gains a test that asserts the host goes GPU-quiet (no new shmem frames, pass-timings stop
advancing) a short time after the last mutation, and re-arms on the next edit.

## Skip the render correctly

When skipping, the loop must still:

- **process every control-plane command** each iteration (Unity's "input ticks every frame, render
  skips" — the control plane stays snappy even while the GPU idles),
- **not** advance frame-indexed temporal state (TAA jitter, SSGI frame index) — a skipped frame is not
  a rendered frame,
- re-publish/keep the last shmem frame valid so the editor subsurface keeps showing the converged image.

## Done when

- A static scene with a static camera drives the host GPU to idle (re-presenting the last frame); `sa
  pass-timings` shows the GPU passes stop advancing; `nvidia-smi` util/power drop off the pegged max.
- Any edit, camera move, or playing animation immediately resumes rendering at `target_fps`.
- `SAFFRON_MAX_FPS`, `max_fps_from_env`, and the editor env branch no longer exist; `pace_loop` reads
  `target_fps`.
- No hard-stop: idle holds a non-zero heartbeat; first post-interaction frame is not janky.
- `just engine` + `just prepare-for-commit` clean; e2e idle/re-arm test green; docs page for the
  reactive loop added.
