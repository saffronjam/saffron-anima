+++
title = 'Render quality tiers'
weight = 9
+++

# Render quality tiers

A render quality tier is one named knob — `low`, `medium`, `high`, or `ultra` — that expands to the
per-effect parameters of the scalable screen-space GI stack: SSGI, GTAO, and contact shadows. It
replaces the old per-effect on/off toggles with a single source of truth, so the editor viewport can
run a cheaper tier than a shipped game and the exported game can expose the tier as its
graphics-settings slider.

The screen-space GI stack is the expensive part of a frame and its cost is **resolution-bound, not
scene-bound** — SSGI alone is the single most expensive pass. A near-empty scene pays almost the same
as a full one, so the way to make a frame cheaper is to dial the effects down, not to simplify the
scene. The tier is that dial.

## What a tier sets

Each preset resolves to a [`RenderQuality`] the renderer applies to [`Ssao`]. The parameters it
drives are the ones already carried as runtime push-constants, so changing a tier costs nothing — no
shader recompile, no target rebuild:

| Tier | SSGI | GTAO | Contact shadows | SSGI steps |
|---|---|---|---|---|
| `low` | off | off | off | — |
| `medium` | on | on | off | 4 |
| `high` | on | on | on | 8 |
| `ultra` | on | on | on | 12 |

`low` disables the screen-space stack entirely (direct + image-based lighting only) — the cheapest
interactive mode, mirroring how Unreal's Low scalability disables Lumen. `high` is the engine's
historical look and the default. A `custom` tier is the escape hatch: it carries hand-set parameters
rather than a preset (resolving from `high` as its base).

> [!NOTE]
> The tier covers the **scalable** screen-space effects. Architectural on/off switches (clustered
> lighting, IBL, DDGI, RT shadows, ReSTIR) stay as their own toggles — they are capability choices,
> not quality dials. SSGI *ray count* and a half-resolution path are deeper (shader + target) changes
> tracked separately; the tier dials the step counts and enable flags that are already push-constants.

## Driving it

One control command sets the tier and one reads it back; both return the resolved per-effect state so
a caller (the editor's Render panel, the `sa` CLI, a game settings menu) sees what the tier means:

```sh
sa set-render-quality medium   # → { tier: "medium", ssgi: true, gtao: true, contactShadows: false }
sa get-render-quality          # → the active tier + resolved flags
```

An unknown tier name is a typed error, not a silent default. The active tier is saved with the
project (the `renderSettings.quality` block) and reported in `render-stats` (the `quality` field, the
knob the `ssao`/`ssgi`/`contactShadows` telemetry bools derive from), so the editor's Render panel
shows it as a dropdown beside anti-aliasing.

## Auto-quality (frame-budget controller)

The tier can also drive itself. With `auto_quality` on (a `set-perf-config` field, off by default), a
frame-budget controller watches each frame's work time against the budget (`1000 / target_fps`) and
steps the tier to hold it: a sustained run of over-budget frames steps **down** (cheaper GI), a
sustained run with comfortable headroom steps back **up**, and a single hitch ≥ 2× budget steps down
at once. Hysteresis (consecutive-frame thresholds + a post-switch cooldown) stops it oscillating; it
never auto-selects `ultra` (a deliberate stills tier) or drops below `low`. It reuses the tier as its
only actuator — a step is just a `set-render-quality` — so it adds no new render path. This is the
safe, self-contained form of a frame-budget controller; scaling the offscreen *resolution* to the
budget is a deeper, separate change.

## In the code

| What | File | Symbols |
|---|---|---|
| Tier → parameters | `rendering/src/quality.rs` | `QualityTier`, `RenderQuality`, `resolve`, `from_name` |
| Apply to the stack | `rendering/src/ssao.rs` | `Ssao::apply_quality` (SSGI / contact step pushes) |
| Renderer knob | `rendering/src/renderer.rs` | `set_render_quality`, `render_quality` |
| Control commands | `control/src/commands_render.rs` | `set-render-quality`, `get-render-quality`, `render_quality_result` |
| Project save/load | `rendering/src/render_settings.rs` | `RenderSettings::quality` |
| Editor UI | `editor/src/panels/RenderPanel.tsx` | the Quality `Select` |
| Auto-quality controller | `rendering/src/budget.rs` | `BudgetController`; `PerfConfig::auto_quality` |

## Related

- [ssgi](../ssgi/) — the most expensive pass the tier dials
- [gtao](../gtao/) and [contact-shadows](../contact-shadows/) — the other tier-gated effects
- [Reactive loop](../../app-lifecycle-and-window/main-loop-and-run/) — idles a static viewport; the
  tier reduces the cost of the frames that *do* render
