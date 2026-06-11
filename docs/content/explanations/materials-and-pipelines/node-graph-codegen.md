+++
title = 'Node-graph codegen'
weight = 6
+++

# Node-graph codegen

A material can be authored as a node graph — constants, texture samples, and math wired into a surface — the way Unreal's material editor works. The graph is stored on the [material asset](../native-materials/) and turned into a shader two different ways depending on what it contains. This is the engine's endgame for material authoring: arbitrary procedural surface math, without hand-writing Slang and without exploding the pipeline count.

## The data model

A graph is plain JSON on the `.smat`:

```jsonc
{
  "nodes": [
    { "id": "c",   "type": "constant",   "props": { "value": [1,0,0,1] } },
    { "id": "t",   "type": "textureSlot","props": { "slot": "albedo" } },
    { "id": "mul", "type": "multiply" },
    { "id": "out", "type": "materialOutput" }
  ],
  "edges": [
    { "from": ["c","rgba"],   "to": ["mul","a"] },
    { "from": ["t","rgba"],   "to": ["mul","b"] },
    { "from": ["mul","rgba"], "to": ["out","baseColor"] }
  ]
}
```

Node `type` strings match the emitter's switch in `assets.cppm` and the editor palette in `materials/graph.ts` — the two are kept in sync by hand. Editor-only data (a node's canvas position) rides in `props.editorPos`, which the engine ignores.

## Fold or codegen

The key decision is whether a graph needs a *new shader* at all.

```mermaid
flowchart TD
    G["material graph"] --> L["lowerGraphToParams"]
    L --> Q{"all nodes fold<br/>to params?"}
    Q -- "yes (constant→param,<br/>texture→slot)" --> P["write MaterialParams<br/>(no recompile)"]
    Q -- "no (multiply, frac,<br/>uv, …)" --> E["emitGraphSurface<br/>→ Slang evalSurface body"]
    E --> S["slangc → SPIR-V"]
    S --> PSO["per-graph PSO"]
    P --> R["render"]
    PSO --> R
```

A graph that is just a constant feeding `baseColor`, or a texture feeding a slot, **folds**: its values become `MaterialParams` and it draws on the shared übershader — no compile. A graph with procedural math (`multiply`, `lerp`, `frac`, `uv`, …) **cannot** fold; it is lowered to a Slang `evalSurface` body and compiled.

This is the cost model: editing a *constant* is a buffer write; changing graph *topology* is a recompile. The editor folds first and only pays slangc when the graph genuinely needs it.

## Emitting and compiling

`emitGraphSurface` walks the nodes in array order (inputs precede consumers), emitting one typed Slang statement per node — `float4 n_<id> = …` — then assigns the `materialOutput` channels. The supported set is the [node library](#the-node-library). `compileMaterialGraph` / `compileMaterialPreviewShader` splice that body into a self-contained shader (the preview variant matches `PreviewPush` and the sphere vertex layout), then shell out to `slangc` (`findSlangc` locates it via `SAFFRON_SLANGC`, the prebuilt cache, or `PATH`). The result is a per-graph `.spv`.

The preview path is wired end to end: `preview-render` detects a non-foldable graph, codegens its shader, builds a per-graph pipeline (`newPreviewPipeline` is parameterized by spv path), and renders the procedural surface on the sphere. Running it produces a validation-clean image — the full `graph → Slang → slangc → PSO → pixels` pipeline.

> [!NOTE]
> Runtime `slangc` is an **editor** capability. Shipped builds bake material SPIR-V at cook time (planned); the runtime-compile path stays behind the editor.

## The node library

Math/utility nodes operate on `float4` values, wired by pin name (`a`/`b`/`t`): `multiply`, `add`, `subtract`, `divide`, `lerp`, `saturate`, `oneMinus`, `dot`, `step`, `smoothstep`. Procedural nodes — `uv`, `sin`, `cos`, `frac` — read the surface UV, so a `frac(uv * 8)` graph renders a repeating pattern. Leaf nodes are `constant` and `textureSlot`; the sink is `materialOutput` (`baseColor`, `metallic`, `roughness`, `normal`, `emissive`). An unknown node emits a safe `float4(0)`.

## The editor

The React Flow view (`MaterialGraphEditor`) is a full-screen canvas over the live preview: a categorized palette adds nodes, drag wires pins, and edits **auto-apply** (debounced) through `material-set-graph` → `preview-render` so the sphere morphs as you work. A *Compile* button forces codegen (`material-compile-graph`). `material-get` returns the stored graph, so reopening a material loads its canvas.

## In the code

| What | File | Symbols |
|---|---|---|
| Fold vs codegen | `assets.cppm` | `lowerGraphToParams` |
| Slang emitter | `assets.cppm` | `emitGraphSurface` |
| Compile + locate slangc | `assets.cppm` | `compileMaterialGraph`, `compileMaterialPreviewShader`, `findSlangc` |
| Preview render-wiring | `renderer_thumbnail.cpp` | `renderMaterialPreview`, `newPreviewPipeline` |
| Control commands | `control_commands_asset.cpp` | `material-set-graph`, `material-compile-graph`, `preview-render` |
| Editor model + palette | `editor/src/materials/graph.ts` | `NODE_SPECS`, `graphToFlow`, `flowToGraph` |
| Editor canvas | `editor/src/panels/MaterialGraphEditor.tsx` | `MaterialGraphEditor`, `SaffronNode` |

## Related

- [Native materials](../native-materials/) — the asset the graph lives on, and the params it folds into
- [Übershader](../ubershader-and-specialization/) — the shared shader foldable graphs draw on
- [Materials & PSOs](../material-and-pso-selection/) — why a per-graph shader still means few pipelines
