+++
title = 'Materials & pipelines'
weight = 7
bookCollapseSection = true
+++

# Materials & pipelines

A material is the surface description a mesh draws with — its shader and its parameters — and a pipeline is the compiled GPU state that renders it. Materials are first-class, editable assets (`.smat`), assignable to entities and authorable in a node graph. These pages explain how a material resolves to a pipeline and how the pipeline count stays small through four mechanisms:

- One übershader covers every material.
- A specialization constant adds the unlit variant.
- A single bindless texture array lets draws that differ only by texture batch together.
- A node graph folds to params where it can, and only codegens a per-graph shader when it must.

## Pages

| Page | Covers | Code |
|---|---|---|
| `material-and-pso-selection` | the `Material` (shader + variant), `requestMeshPipeline`, build-on-miss cache | `renderer_pipelines.cpp` · `requestMeshPipeline` |
| `ubershader-and-specialization` | one `mesh.slang`, `[[vk::constant_id]]` unlit permutation, variants | `renderer_pipelines.cpp`; `mesh.slang` · `kUnlit` |
| `descriptor-sets` | set 0 bindless, set 1 lighting, set 2 instances, set 3 IBL, set 4 screen-space | `mesh.slang` · `vk::binding`; `renderer_types.cppm` |
| `bindless-textures` | one albedo array (partiallyBound + updateAfterBind), `uploadTexture` slot, per-instance index | `renderer_textures.cpp` · `uploadTexture`; `mesh.slang` |
| `native-materials` | `.smat` assets, the params buffer + `evalSurface` seam, PBR slots, instances, the editor | `assets.cppm` · `MaterialAsset`; `mesh.slang` · `evalSurface` |
| `node-graph-codegen` | graph fold-vs-codegen, the Slang emitter, `slangc` → per-graph PSO, the React Flow editor | `assets.cppm` · `emitGraphSurface`; `MaterialGraphEditor.tsx` |
