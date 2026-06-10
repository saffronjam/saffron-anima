# Phase 17 — Node-graph data model

**Status:** NOT STARTED
**Depends on:** 10

## Goal

Define the material **graph** data model (nodes, typed pins, edges, the output/material node), store it
(embedded in the `.smat` or a sibling `.smatg`), add its control DTOs, and implement a **params-only
lowering** first: a graph whose nodes fold to constant parameter values / feature bits drives the existing
fixed `evalSurface` with **no codegen**. This proves the round-trip and the editor data flow before the
codegen backend (phase 18) exists.

## Why

Building the graph in two steps de-risks it: get the data model, serialization, control plane, and (phase
20) the React Flow editor working against a *parameters-only* backend that needs no `slangc`, then swap in
real codegen (phase 18) underneath the same data model. A params-only graph is limited (no procedural
math) but it exercises everything except the compiler.

## Design

```jsonc
// graph (in .smat under "graph", or sibling .smatg)
{
  "version": 1,
  "nodes": [
    { "id": "n1", "type": "texture", "props": { "asset": "<uuid>", "colorspace": "srgb" }, "pos": [x,y] },
    { "id": "n2", "type": "constant", "props": { "value": 0.5 } },
    { "id": "out", "type": "materialOutput" }
  ],
  "edges": [ { "from": ["n1","rgb"], "to": ["out","baseColor"] }, { "from": ["n2","r"], "to": ["out","roughness"] } ]
}
```

- **Typed pins**: `float`, `float2/3/4`, `texture` (sampler ref). The `materialOutput` node's inputs are
  exactly the `SurfaceData`-producing channels (Base Color, Metallic, Roughness, Normal, Emissive,
  Occlusion, Height, Opacity). Edges connect compatible types (swizzle/scalar-broadcast rules defined here).
- **Params-only lowering** (this phase): a graph is "foldable" iff every path to an output is a constant /
  texture-sample / channel-pick the fixed `evalSurface` already supports — i.e. it reduces to the phase-05
  parameter set (texture indices + factors + feature bits). The lowerer walks the DAG and, if foldable,
  emits a `MaterialParamsData`; if not foldable, it's flagged "needs codegen" (phase 18) and renders with
  the last-good/fallback. This keeps phase 17 shippable without a compiler.
- **Master/instance**: a graph lives on a master material; instances (phase 16) still override leaf params.

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `MaterialGraph` struct (nodes/edges/typed pins) + to/from-JSON;
  store under the `.smat` (or `.smatg`); a `lowerGraphToParams(graph) -> optional<MaterialParamsData>`
  (returns nullopt when not foldable).
- `engine/source/saffron/control/control_dto.cppm` + commands — `material.get/update` carry the graph;
  a `material.setGraph {id, graph}` (validate types + acyclicity).
- `tools/gen-control-dto/gen.ts` — regenerate for the graph DTOs.

## Steps

1. Define `MaterialGraph` + typed pins + the `materialOutput` channel set; JSON round-trip.
2. Type/cycle validation on `setGraph`.
3. `lowerGraphToParams` for the foldable subset (constant, texture, channel-pick, scalar math on constants).
4. Wire a foldable graph through resolve so it drives `evalSurface` exactly like a slot material; e2e the
   round-trip + a foldable lowering.

## Gate / done

- `make engine` + `make schema` clean; a foldable graph round-trips and renders identically to the
  equivalent slot material. Non-foldable graphs are flagged (not crash). `make prepare-for-commit` clean.
- Docs: the material graph model + the foldable/codegen distinction.

## Risks

- **Scope creep**: resist building codegen here. The whole point is to prove the data model against a
  no-compiler backend. The foldable check must be honest — anything procedural is "needs codegen", not a
  silent wrong result.
- **Type system**: keep pin types minimal (scalar/vecN/texture) with explicit swizzle rules; a rich type
  system is unnecessary and slows the editor.
