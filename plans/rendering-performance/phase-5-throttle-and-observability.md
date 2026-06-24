# Focus/occlusion throttling + idle/tier observability

**Status:** CORE COMPLETE (observability + occlusion suppression); editor focus-events unverified live
**Scope:** Editor
**Depends on:** Phase 1 (reactive loop + override stack), Phase 2 (shadow-cache state to report)

> **Done.** The reactive-loop state now surfaces and the viewport throttles when hidden:
> - **Observability:** a `ReactiveState` mirror on the renderer (`rendering/src/reactive.rs`) that the
>   host pushes each `on_update` (`set_reactive_state` ← `RedrawController::is_idle`/`converged`/
>   `reasons`), reported in `render-stats` as `idle` / `converged` / `redrawReasons` / `powerState`.
>   `RedrawController::is_idle` returns the *actual* last render verdict (tracks `last_rendered`), so
>   it accounts for keep-warm + suppression, not just the activity flags. **Live-validated**: a static
>   empty scene reports `idle=true, converged=true, redrawReasons=[]`.
> - **Focus / occlusion throttle:** a `PowerState` (focused/unfocused/occluded) set by
>   `set-viewport-power-state`; the host reads it each frame and `RedrawController::set_suppressed`
>   forces the loop to render nothing while **occluded**. **Live-validated**: occluded + a mutating
>   command → host CPU stays 0% (suppressed); focused → renders again; a bogus state → typed error.
> - **CLI:** `sa set-viewport-power-state` / `sa render-stats` expose both (auto from the manifest).
> - **e2e:** `tests/e2e/reactive-loop.test.ts` asserts idle-on-static, re-arm-on-edit, and
>   occlusion-suppression (the deferred Phase-1 idle assertion). `toggles.test.ts` updated for the
>   tier replacing the GI toggles. The e2e *run* needs a non-contended `just e2e` (the documented
>   session-contention caveat); the tests are authored to the harness.
> - **Editor:** `client.setViewportPowerState` + an `App.tsx` focus/blur/visibility listener send the
>   power state. **Unverified live** (needs a `just run` Tauri session) — the engine side is validated.
>
> **Deferred:** the `unfocused` → low-fps *rate* throttle (UE's ~3 fps backgrounded) — currently
> `unfocused` renders on demand like focused; only `occluded` suppresses. A rate cap needs the pacer
> to take a per-state target; small follow-on. The HUD widget showing the readout is display-only and
> left for the editor pass (the data is in `render-stats`).

## Goal

Throttle the host further when the editor window is unfocused, occluded, or minimized — the single
biggest idle win mature editors ship — and make the whole reactive/converge/tier model **visible and
testable** from a shell and the HUD, so the idle behaviour can be asserted in e2e and debugged at a
glance.

## Design

### Focus / occlusion / minimized throttling

The present-only host has no window of its own — the editor (Tauri/React) is the only possible source
of focus and visibility events. Plumb a control-plane message (`viewport.power-state` with
`focused` / `unfocused` / `occluded` / `minimized`) from the editor's `src-tauri` focus/visibility
handlers to a host-side power-mode flag that feeds the Phase-1 pacer and override stack:

- **unfocused** → drop to ~3–10 fps, suspend speculative convergence bursts,
- **occluded / minimized** → skip rendering entirely (re-present nothing; the subsurface is not
  visible), wake on focus regain through the Phase-1 keep-warm path.

Precedent: UE "Use Less CPU when in Background" / `bThrottleCPUWhenNotForeground` (~3–4 fps backgrounded;
auto 60 fps cap on battery); Godot self-limits to ~10 fps unfocused via a separate unfocused
`sleep_usec`; Bevy `WinitSettings.unfocused_mode = ReactiveLowPower` ("zero resources when minimized").
This is cheap once Phases 1–2 exist — it is mostly a new event source feeding the existing pacer.

### Observability over `sa` CLI + HUD

Expose the reactive state so it is verifiable (AGENTS.md keep-current rule). Alongside
`get-perf-config` (`control/src/commands_render.rs`), report:

- effective fps and current power-state (focused/unfocused/occluded),
- `needs_redraw` and the active **override-stack reasons** (which subsystem is keeping the GPU hot),
- per-effect `converged` flags (from Phase 4),
- current quality tier (from Phase 3),
- per-light shadow cache hit/miss this frame (from Phase 2),
- a `viewport.invalidate` command (force a redraw / bump change-gen) so tests can re-arm accumulation.

Surface the same on the HUD next to the existing `frame_history` grading (precedent: Unity
`OnDemandRendering.effectiveRenderFrameRate`; Godot "Show Update Spinner"; UE path-tracer convergence
readout). Add the matching `sa` subcommand.

### e2e

The driver asserts the end-to-end reactive story: boot headless, hold the scene static → GPU goes quiet
(pass-timings stop advancing, no new shmem frames, GI/shadow cost decays); send a mutation → renders
resume and accumulation re-arms; send `unfocused` → frame rate drops to the throttle floor.

## Done when

- Unfocusing/minimizing the editor visibly drops host GPU util/power (`nvidia-smi`); refocusing wakes it
  with no jank (Phase-1 keep-warm).
- `sa` reports power-state, override reasons, converged flags, tier, and shadow cache state; the HUD
  shows the same.
- e2e idle/re-arm/throttle test green.
- `just engine` + `just prepare-for-commit` clean; docs reactive-loop page extended with the
  throttle/observability surface.
