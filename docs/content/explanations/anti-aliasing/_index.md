+++
title = 'Anti-aliasing'
weight = 13
bookCollapseSection = true
+++

# Anti-aliasing

Anti-aliasing smooths the jagged edges that arise when continuous geometry is sampled onto a
discrete grid of pixels. Saffron offers three techniques, each trading quality for cost and
switchable at runtime with `se set-aa`:

- **MSAA** cleans geometry edges by multisampling.
- **FXAA** blurs luma edges in a cheap post-process.
- **TAA** reuses history for a temporal solve, covered in
  [Screen-space & post](../screen-space-and-post/).

## Pages

| Page | Covers | Code |
|---|---|---|
| `msaa` | sample count baked into PSOs, the graph resolve attachment | `renderer_aa.cpp`; `render_graph.cppm` · `RgAttachment.resolve` |
| `fxaa` | luma edge detection on a 1× scratch → offscreen | `fxaa.slang`; `renderer_aa.cpp` |
| `aa-modes` | `setAa(off\|fxaa\|msaa2\|4\|8)`, mutual exclusivity, PSO rebuild | `renderer_aa.cpp` · `setAa` |
