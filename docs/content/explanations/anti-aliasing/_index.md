+++
title = 'Anti-aliasing'
weight = 13
bookCollapseSection = true
+++

# Anti-aliasing

Anti-aliasing smooths the jagged edges that arise when continuous geometry is sampled onto a
discrete grid of pixels. Anima offers three techniques, each trading quality for cost and
switchable at runtime with `sa set-aa`:

- **MSAA** cleans geometry edges by multisampling.
- **FXAA** blurs luma edges in a cheap post-process.
- **TAA** reuses history for a temporal solve, covered in
  [Screen-space & post](../screen-space-and-post/).

## Pages

| Page | Covers | Code |
|---|---|---|
| `msaa` | sample count baked into PSOs, the graph resolve attachment | `aa.rs`, `view_target.rs`, `render_graph.rs` · `Aa::sample_count`, `RgAttachment.resolve` |
| `fxaa` | luma edge detection on a 1× scratch → offscreen | `fxaa.slang`, `renderer.rs` · `add_fxaa_pass` |
| `aa-modes` | `Aa::set(off\|fxaa\|msaa2\|4\|8)`, mutual exclusivity, PSO rebuild | `aa.rs` · `Aa::set`, `Aa::mode` |
