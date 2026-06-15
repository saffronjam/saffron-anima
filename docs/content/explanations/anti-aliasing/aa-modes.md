+++
title = 'AA modes'
weight = 3
+++

# AA modes

Anti-aliasing reduces the jagged edges that appear where a triangle's boundary crosses a pixel grid.
A renderer can address it at different points in the pipeline, and each point is a distinct mode with
its own cost and quality.

Saffron offers three approaches and treats them as mutually exclusive: at most one is active at a
time. A single configuration call selects the active mode, and a CLI command fronts it for scripting
and inspection. The host starts at MSAA 4× (clamped to what the device supports); a loaded
project's saved [render settings](../../geometry-and-assets/project-serialization/) override it.

The editor gizmo overlay is unaffected by the active mode — it draws after the resolve and
[anti-aliases itself analytically](../../ui-and-editor/gizmo/).

## The modes

The three approaches differ in where they sample and how they combine:

| Mode | `set-aa` arg | What it is | Where |
|---|---|---|---|
| Off | `off` | no anti-aliasing | — |
| MSAA 2× / 4× / 8× | `msaa2` / `msaa4` / `msaa8` | multisampled scene color + depth, resolved into the offscreen | [MSAA](../msaa/) |
| FXAA | `fxaa` | luma-edge blur, one compute pass on a 1× scratch → offscreen | [FXAA](../fxaa/) |
| TAA | `taa` | history reprojection + neighbourhood clamp + exponential blend | [TAA](../../screen-space-and-post/taa/) |

## Selecting a mode

`setAa(renderer, msaaSamples, fxaa, taa)` takes a sample count plus FXAA and TAA flags and resolves
them into one active mode. MSAA wins if a sample count above 1 is requested; otherwise FXAA, then
TAA. The count maps to a `vk::SampleCountFlagBits` and clamps to the device's `maxSampleCount`, so
asking for `msaa8` on hardware that tops out at 4× yields 4×.

The `sa set-aa` command parses a mode string into those three arguments:

| String | `msaaSamples` | `fxaa` | `taa` |
|---|---|---|---|
| `off` | 1 | false | false |
| `msaa2` / `msaa4` / `msaa8` | 2 / 4 / 8 | false | false |
| `fxaa` | 1 | true | false |
| `taa` | 1 | false | true |

## What a switch does

Switching modes is a full reconfigure, not a flag flip. `setAa` waits for the GPU to go idle, since
the targets and pipeline state objects are about to be destroyed. It stores the new sample count and
flags, then recreates three target sets: the multisampled MSAA pair, the 1× scratch that FXAA and
TAA share, and the TAA motion + history pair. Finally it clears the PSO cache and rebuilds the
depth-prepass pipeline, because the mesh and prepass PSOs bake the sample count.

## Reading back the active mode

`aaMode(renderer)` reports the current mode as a string, and `sa render-stats` exposes it as the
`aa` field. FXAA and TAA report by their flag; otherwise the sample count decides — `"off"` at 1,
`"msaaN"` above.

## In the code

| What | File | Symbols |
|---|---|---|
| Configure + clamp + rebuild | `renderer_aa.cpp` | `setAa`, `setAaMode`, `aaMode` |
| CLI front | `control_commands_render.cpp` | `set-aa`, `render-stats` · `aa` |
| Mode flags | `renderer_types.cppm` | `sampleCount`, `maxSampleCount`, `fxaaEnabled`, `taaEnabled` |

## Related

- [MSAA](../msaa/) — the rasterization-time mode
- [FXAA](../fxaa/) — the cheap post-process mode
- [TAA](../../screen-space-and-post/taa/) — the temporal mode
