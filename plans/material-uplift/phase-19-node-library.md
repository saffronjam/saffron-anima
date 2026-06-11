# Phase 19 — Node library

**Status:** IN PROGRESS — emitter node-set done + visibly rendering; editor palette (phase 20) + render-context nodes remain
**Depends on:** 18

> **Done (codegen node-set, visibly rendering).** `emitGraphSurface` supports the core authoring
> vocabulary: `constant`, `textureSlot`, `multiply`, `add`, `subtract`, `divide`, `lerp`,
> `saturate`/`clamp`, `oneMinus`, `dot`, plus the **procedural** nodes `uv`, `sin`, `cos`, `frac`, `step`,
> `smoothstep` — each emits a typed Slang statement (all values `float4`, wired by pin name a/b/t); an
> unknown node emits a safe `float4(0)` default. e2e `material_nodes.test.ts` proves a
> lerp→oneMinus→saturate graph codegens to compilable Slang; `material_procedural.test.ts` proves a
> `frac(uv * 8)` graph **renders a pattern** through the preview codegen path (valid PNG, validation-clean).
> **Remaining:** (1) render-context nodes (triplanar, noise, Fresnel, normal-map, custom-Slang) need the
> full shader context (world pos/normal/view) — available once scene-path codegen lands (phase-18
> follow-on); (2) the editor **node palette** + custom node components are part of the React Flow editor
> (phase 20); (3) per-node golden compile tests.

## Goal

A serious, extensible library of material nodes — each a **typed Slang snippet** the codegen backend
(phase 18) emits — covering the surface-authoring vocabulary artists expect: textures, UVs, math, vectors,
normal blending, triplanar, noise, Fresnel, time, vertex color, panner, and a raw custom-Slang escape hatch.

## Why

The codegen backend is only as useful as its nodes. This is where "full serious" lives. A clean node
definition format (metadata + typed pins + a Slang emit template) makes the library data-driven and lets
the same definition feed both the C++ emitter and the React Flow palette (phase 20).

## Design

- **Node definition** (shared schema, consumed by the emitter and the editor): `{ type, category, pins:
  [{name, type, default}], outputs: [{name, type}], slang: "<template with $in/$out>", props: [...] }`.
  The emitter substitutes wired inputs / constant props into the template and declares the outputs.
- **The initial library**:
  - *Inputs*: `texture` (bindless sample, colorspace-aware), `constant` (float/vecN), `vertexColor`, `uv`
    (uv0 + tiling/offset/rotate), `time`, `cameraVector`, `worldPosition`, `normalVector`.
  - *Math*: `add sub multiply divide`, `lerp`, `clamp saturate`, `power`, `dot cross normalize length`,
    `min max abs floor frac`, `oneMinus`, `remap`.
  - *Vectors*: `combine` (make vecN), `split`/`swizzle`, `append`.
  - *Surface*: `normalMap` (tangent-space decode+strength), `normalBlend` (RNM/UDN), `triplanar` (world-space
    projection — the classic "no UVs needed" node), `heightToNormal`.
  - *Procedural*: `noise` (perlin/simplex/voronoi/fbm), `gradient`, `checker`, `panner`/`rotator`.
  - *Utility*: `fresnel` (Schlick), `desaturate`, `customSlang` (raw expression escape hatch — the power-user
    valve; sandboxed to the `MaterialInput`/intrinsics scope).
  - *Output*: `materialOutput` (the `SurfaceData` channels).
- **Type safety**: pins carry types (phase 17); the editor blocks incompatible wires; scalar→vec broadcast
  and explicit swizzle are the only implicit conversions.

## Files to touch

- `engine/source/saffron/` (the material compiler area) — the node-definition registry + Slang emit
  templates per node; the emitter (phase 18) consumes them. Keep definitions in one data-driven table so
  adding a node is one entry + a template.
- A shared node-catalog export (JSON) the editor reads for its palette (phase 20), generated from the same
  registry (or hand-mirrored + tested for parity).
- `tests/` — per-node codegen tests: each node's snippet compiles in isolation and produces expected output
  for known inputs.

## Steps

1. Define the node-definition format + the emit-template substitution rules.
2. Implement the initial library (start with inputs/math/output, then surface/procedural).
3. Export the node catalog for the editor; add parity tests (C++ registry vs editor catalog).
4. Per-node codegen tests (compile + golden output where feasible).
5. e2e: a triplanar + noise material compiles and renders plausibly.

## Gate / done

- `make engine` clean; every library node compiles in isolation and in a representative graph.
- The editor palette (phase 20) lists the full catalog. `make prepare-for-commit` clean.
- Docs: the node reference (categories + each node's purpose) — a real reference page.

## Risks

- **`customSlang` safety**: a raw-code node can emit anything; scope it to a documented intrinsic/`MaterialInput`
  surface and let `slangc` reject the rest (compile errors surface to the editor). Don't try to sandbox
  perfectly — fail-to-compile is acceptable.
- **Catalog parity**: the C++ emitter and the editor palette must agree on pins/types; generate one from the
  other or test parity, or they silently drift.
- **Noise cost**: procedural noise is expensive per-fragment; document it and prefer baking where static.
