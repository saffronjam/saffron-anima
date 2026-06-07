+++
title = 'Materials & pipelines'
weight = 7
bookCollapseSection = true
+++

# Materials & pipelines

A material is the surface description a mesh draws with — its shader and its parameters — and a pipeline is the compiled GPU state that renders it. These pages explain how a material resolves to a pipeline and how the pipeline count stays small through three mechanisms:

- One übershader covers every material.
- A specialization constant adds the unlit variant.
- A single bindless texture array lets draws that differ only by texture batch together.

## Pages

| Page | Covers | Code |
|---|---|---|
| `material-and-pso-selection` | the `Material` (shader + variant), `requestMeshPipeline`, build-on-miss cache | `renderer_pipelines.cpp` · `requestMeshPipeline` |
| `ubershader-and-specialization` | one `mesh.slang`, `[[vk::constant_id]]` unlit permutation, variants | `renderer_pipelines.cpp`; `mesh.slang` · `kUnlit` |
| `descriptor-sets` | set 0 bindless, set 1 lighting, set 2 instances, set 3 IBL, set 4 screen-space | `mesh.slang` · `vk::binding`; `renderer_types.cppm` |
| `bindless-textures` | one albedo array (partiallyBound + updateAfterBind), `uploadTexture` slot, per-instance index | `renderer_textures.cpp` · `uploadTexture`; `mesh.slang` |
