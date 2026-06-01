+++
title = 'Render commands'
weight = 4
+++

# Render commands

The render commands flip renderer feature switches and read back the last frame's draw counters. Each is a thin shell over one renderer `set*`/`*Enabled` accessor, and each toggle returns the resolved state so a script can confirm what actually took effect.

## Liveness and stats

| Command | Params | Effect |
|---|---|---|
| `ping` | — | Returns `{pong, engine, version, pid}`. Liveness check and engine info. |
| `help` | — | Lists every registered command with its one-line help (registry order). |
| `render-stats` | — | Returns last frame's scene counters plus the state of every render feature. |

`render-stats` is the broad read: draw calls, batches, instances, and a flag for each toggleable feature — `clustered`, `depthPrepass`, `shadows`, `ibl`, `ssao`, `contactShadows`, `ssgi`, `ddgi`, `rtSupported`, `rtShadows`, `restir`, `blasCount`, `pipelines`, `hdr`, `exposureEv`, and `aa`.

## Feature toggles

Most toggles take a `{0|1}` (also accepting `true`/`false`/`off`) and report back the boolean. A few take an enum string.

| Command | Params | Effect |
|---|---|---|
| `set-aa` | `{off\|fxaa\|taa\|msaa2\|msaa4\|msaa8}` | Anti-aliasing mode. Decodes to sample count + fxaa/taa flags. |
| `set-clustered` | `{0\|1}` | Clustered (Forward+) light culling vs. a brute-force loop. |
| `set-ibl` | `{0\|1}` | Image-based ambient vs. flat ambient. |
| `set-ssao` | `{0\|1}` | Screen-space ambient occlusion (GTAO). |
| `set-contact-shadows` | `{0\|1}` | Screen-space contact shadows. |
| `set-ssgi` | `{0\|1}` | Screen-space one-bounce global illumination. |
| `set-gi` | `{off\|ddgi}` | DDGI probe global illumination (multi-bounce). |
| `set-shadows` | `{0\|1}` | The directional shadow map. |
| `set-rt-shadows` | `{0\|1}` | Hardware ray-query shadows. Errors unless `rtSupported`. |
| `set-restir` | `{0\|1}` | ReSTIR stochastic many-light direct. Errors unless `rtSupported`. |
| `set-exposure` | `{ev}` | Tonemap exposure in stops; the renderer raises 2 to it. |
| `set-depth-prepass` | `{0\|1}` | The vertex-only depth pre-pass. |

## The toggle shape

The boolean toggles share one parse block: a number is true when non-zero, a bool is itself, and a string is false only for `0`/`false`/`off`. Then they call the renderer setter and read the state straight back.

```cpp
setSsao(ctx.renderer, enabled);
return json{ { "ssao", ssaoEnabled(ctx.renderer) } };
```

Returning the queried state rather than the requested value is deliberate. A feature the hardware cannot provide reports its real result: ray-query shadows and ReSTIR both gate on `rtSupported(ctx.renderer)` up front and error if ray tracing is unavailable, so a script never silently believes it turned RT on. `set-aa` is the one enum toggle — it maps the mode string to a sample count plus `fxaa`/`taa` flags, rejects anything else, and returns the renderer's canonical `aaMode` string.

The expectation is that a new render feature ships its `set*`/`*Enabled` pair *and* a matching command, so the editor stays drivable and visually debuggable via a [screenshot](../screenshots-and-capture/) after the toggle. `render-stats` is where each new flag surfaces.

## In the code

| What | File | Symbols |
|---|---|---|
| Registration | `control_commands_render.cpp` | `registerRenderCommands` |
| Stats read-back | `control_commands_render.cpp` | `render-stats`; `renderStats`, the `*Enabled` accessors |
| AA enum decode | `control_commands_render.cpp` | `set-aa`; `setAa`, `aaMode` |
| RT gating | `control_commands_render.cpp` | `set-rt-shadows`, `set-restir`; `rtSupported` |
| Exposure | `control_commands_render.cpp` | `set-exposure`; `setExposure`, `exposureEv` |

## Related
- [Tonemapping and exposure](../../screen-space-and-post/tonemap-and-exposure/) — what `set-exposure` drives
- [Capture](../screenshots-and-capture/) — confirm a toggle visually
- [Control plane](../control-plane-architecture/) — registration and dispatch
