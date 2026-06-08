# Phase 1 — debug-utils pass labels

**Status:** NOT STARTED

The cheapest, most independent win: make every render-graph pass *named* in external capture tools.
Today a RenderDoc or Nsight capture of `SaffronEngine` shows an anonymous wall of `vkCmdDraw*` /
`vkCmdDispatch` with no pass boundaries. Wrapping each pass body with a `VK_EXT_debug_utils` label
makes the whole frame self-documenting in every standard tool — at zero behavioral cost and with no
dependency on any later phase. This is the in-engine analogue of UE's `SCOPED_DRAW_EVENT`: a *marker*
for capture tools, kept strictly distinct from the *timing* scopes the later phases add.

## What to add

`executeRenderGraph` (`render_graph.cppm:297-419`) already walks `graph.passes` in order and brackets
each `RgPass.execute` body — the same seam the GPU timestamps use (`render_graph.cppm:309,406`). Emit a
label around that body:

- `cmd.beginDebugUtilsLabelEXT({ .pLabelName = pass.name.c_str() })` immediately before the pass body
  (outside the timestamp `eTopOfPipe` write is fine — labels and timestamps are independent), and
  `cmd.endDebugUtilsLabelEXT()` immediately after.
- Optionally give each label a stable color derived from a hash of `pass.name`, so groups read
  consistently across captures (cosmetic; RenderDoc honors `color[4]`).
- Emit **regardless of `ProfilerMode`** — labels are free and useful even when timing is `Off`. This is
  the one piece of the plan that is always-on.

## Extension wiring

`VK_EXT_debug_utils` is an *instance* extension whose command function pointers
(`vkCmdBeginDebugUtilsLabelEXT` / `vkCmdEndDebugUtilsLabelEXT`) must be resolved. Resolve them next to
the existing extension/function-pointer resolution in device/instance creation
(`renderer.cppm` device-create block, near the `timestampPeriod`/`pipelineStatsSupported` reads at
`renderer.cppm:254-257`):

- Request `VK_EXT_debug_utils` at instance creation if not already present (vk-bootstrap path).
- Resolve the two `pfn` pointers and store them on the rendering context alongside the existing
  ray-tracing/ext function pointers.
- **Guard emission on presence**: if the extension or pointers are absent, the label calls are no-ops —
  no validation noise, no crash. The Vulkan-Hpp dispatcher must be the one carrying these pointers, so
  prefer routing through the engine's existing dispatch loader rather than a raw `pfn` call where the
  codebase already uses Hpp.

Keep these markers **distinct** from the timing scopes added in Phases 2–4 (the UE
`SCOPED_DRAW_EVENT` vs `RDG_GPU_STAT_SCOPE` distinction): a debug-utils label never feeds an in-engine
number, and a timing scope never depends on a label being present.

## Files touched

| What | File | Symbols |
|---|---|---|
| Per-pass label emit | `engine/source/saffron/rendering/render_graph.cppm` | `executeRenderGraph` |
| `pfn` resolution + ext request | `engine/source/saffron/rendering/renderer.cppm` (device/instance create) | debug-utils function pointers |
| Context fields for the `pfn`s | `engine/source/saffron/rendering/renderer_types.cppm` | rendering context / `GpuProfiler` neighbour |
| Concept page | `docs/content/explanations/frame-and-render-graph/` profiler page (new in Phase 9) | — |

## Validation

- `make engine` + `make prepare-for-commit` clean.
- A headless run with validation layers is clean (no `VUID` warnings from the label calls); on a
  device without `VK_EXT_debug_utils` the labels are silently skipped.
- A `make run` + RenderDoc/Nsight capture (or a `vkconfig` capture under llvmpipe) shows named pass
  regions matching `graph.passes` order. Document this manual check in the Phase 9 docs page; no e2e
  assertion is needed since labels carry no queryable state.
