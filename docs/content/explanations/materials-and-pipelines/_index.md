+++
title = 'Materials & pipelines'
weight = 7
+++

# Materials & pipelines

How a material becomes a pipeline, and how the engine keeps the pipeline count tiny. One übershader covers every material, a specialization constant adds the unlit variant, and a single bindless texture array lets draws that differ only by texture batch together.

## Pages

| Page | Covers | Code |
|---|---|---|
| `material-and-pso-selection` | the `Material` (shader + variant), `requestMeshPipeline`, build-on-miss cache | `renderer_pipelines.cpp` · `requestMeshPipeline` |
| `ubershader-and-specialization` | one `mesh.slang`, `[[vk::constant_id]]` unlit permutation, variants | `renderer_pipelines.cpp`; `mesh.slang` · `kUnlit` |
| `descriptor-sets` | set 0 bindless, set 1 lighting, set 2 instances, set 3 IBL, set 4 screen-space | `mesh.slang` · `vk::binding`; `renderer_types.cppm` |
| `bindless-textures` | one albedo array (partiallyBound + updateAfterBind), `uploadTexture` slot, per-instance index | `renderer_textures.cpp` · `uploadTexture`; `mesh.slang` |
