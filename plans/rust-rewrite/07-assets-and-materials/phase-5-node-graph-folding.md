# Phase 5 — node-graph folding and surface emit

**Status:** COMPLETED

**Depends on:** 07-assets-and-materials:phase-2-material-asset-and-serde

## Goal

Port the two pure-CPU node-graph functions: `lower_graph_to_params` (collapse a constant/texture-only
graph into the flat `MaterialAsset` factor/texture fields, returning `false` the moment it hits a
procedural/math node) and `emit_graph_surface(graph, mesh)` (emit the Slang `evalSurface` body — one
statement per node in array order, then the `materialOutput` channel assignments). No `slangc` here —
this phase produces and tests the *strings* and the *fold decision*; phase 6 compiles them.

## Why this shape (NO LEGACY)

Both functions are deterministic JSON-walkers over the graph's `nodes`/`edges` arrays. The folding +
emit logic is a frozen contract with the editor's node-graph model: the channel name strings
(`baseColor`, `emissive`, `metallic`, `roughness`, `emissiveStrength`, `normal`, `height`), the node
type strings (`constant`, `texture`, `textureSlot`, `materialOutput`, math/utility types), the pin
names (`a`/`b`/`t`), and the `mesh`-vs-preview context differences (the `mesh` target emits into the
übershader's `m.mat`/`albedoTextures[]` with a 7-field `SurfaceData`; the preview/self-contained target
emits `mat`/`textures[]` with a 5-field `SurfaceData`) must reproduce byte-for-byte or a graph material
silently miscompiles. The graph is read as `serde_json::Value` (the phase-2 opaque shape). The emitted
Slang is a `String` built with `format!`; the C++ `std::format` calls port directly. Folding is decided
edge-by-edge against the `materialOutput` node; an unrecognized channel or a non-constant/texture source
flips `foldable = false`.

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `lowerGraphToParams` (the `nodes`/`edges` walk, the
  `uuidOf`/`scalar` lambdas, the per-channel constant + texture folding, `foldable = false` on a math
  node), `emitGraphSurface` (the `mesh` flag, the `inputFrom` "node:pin" edge map, the per-node-type
  emit for `materialOutput`/`constant`/`textureSlot`/math, the `m.mat.tex0.*`/`mat.tex.*` slot index
  mapping, the 7-field vs 5-field `SurfaceData`).
- The AGENTS rule: "Node-graph materials are folded when they can be… any procedural/math node forces
  Slang codegen."

## Acceptance gate

- `cargo build -p saffron-assets` + workspace green; clippy + fmt clean.
- `lower_graph_to_params` `#[test]`s: a constant-only graph folds `baseColor`/`emissive`/`metallic`/
  `roughness`/`emissiveStrength` into the right fields and returns `true`; a texture-only graph folds the
  `albedo`/`normal`/`emissive`/`orm`/`height` slots; a graph with a math node returns `false` and leaves
  the flat fields at their pre-fold values; a graph with no `materialOutput` returns `false`.
- `emit_graph_surface` `#[test]`s: a known small graph emits a string equal to a captured golden for
  both `mesh=false` (preview, `mat`/`textures[]`, 5-field `SurfaceData`) and `mesh=true` (übershader,
  `m.mat`/`albedoTextures[]`, 7-field with `worldNormal`/`occlusion`/`opacity`); the empty/absent graph
  emits the default passthrough body; the `textureSlot` index mapping matches per slot name in both
  contexts.
