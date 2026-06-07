+++
title = 'Screen-space & post'
weight = 11
bookCollapseSection = true
+++

# Screen-space & post

Screen-space effects approximate lighting and shading from the rendered image itself, then a final
color step maps the result to the display. A thin G-buffer of view-space normal and depth feeds
ambient occlusion, contact shadows, SSGI, and the temporal passes; tonemapping reduces the linear
HDR scene to the range a display can show.

## Pages

| Page | Covers | Code |
|---|---|---|
| `thin-gbuffer` | view-space normal + depth in one rgba16f MRT target | `gbuffer.slang` |
| `gtao` | horizon-based ambient occlusion, modulating only the indirect term | `gtao.slang`; `mesh.slang` · `aoMap` |
| `contact-shadows` | screen-space ray march that darkens the directional direct term | `mesh.slang` · `contactMap`, `screenFlags.x` |
| `ssgi` | one-bounce screen-space indirect radiance added to the ambient term | `mesh.slang` · `ssgiMap`, `screenFlags.y` |
| `motion-vectors` | camera reprojection velocity for temporal reuse | `motion.slang` |
| `taa` | history reprojection + neighbourhood clamp + exponential blend | `taa.slang` |
| [tonemap-and-exposure](tonemap-and-exposure/) | exposure, Reinhard, gamma 2.2, in-place on the HDR offscreen | `tonemap.slang` |
| `compute-post-process-pattern` | `StorageImageRWCompute`, RMW transitions, dispatch in the graph | `render_graph.cppm` · `RgUsage`; `renderer.cppm` |
