# Focus/occlusion throttling + idle/tier observability

**Status:** NOT STARTED
**Scope:** Editor
**Depends on:** Phase 1 (reactive loop + override stack), Phase 2 (shadow-cache state to report)

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
