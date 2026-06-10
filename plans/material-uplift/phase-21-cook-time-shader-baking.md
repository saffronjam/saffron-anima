# Phase 21 — Cook-time shader baking

**Status:** NOT STARTED
**Depends on:** 18

## Goal

Bake every material graph's generated SPIR-V into the project at build/cook time, so a shipped (non-editor)
runtime loads **precompiled** material shaders and never invokes `slangc`. This preserves SaffronEngine's
"no runtime shader compiler at runtime" property — the node-graph's compile cost stays an editor-time
concern, exactly like the engine's hand-written shaders compile in CMake today.

## Why

Phase 18's runtime `slangc` is an editor convenience. A shipped game must not depend on a shader compiler
being present, must start fast, and must avoid first-use compile hitches. UE solves this with a cooked
shader cache; this is the SaffronEngine equivalent, scoped to material graphs.

## Design

- **A cook step** enumerates every material asset's graph, lowers + compiles each to SPIR-V (reusing the
  phase-18 emitter + `slangc`), for each needed permutation (static/skinned × passes), and writes the
  results into the project's asset bundle keyed by graph hash (the same key the runtime PSO cache uses).
- **Runtime load path**: when a material resolves, the renderer looks up the baked SPIR-V by graph hash in
  the bundle; if present (shipped), load it directly into a PSO; the runtime-`slangc` path is compiled out
  (or behind the editor flag from phase 18). Same cache key → editor-compiled and cook-baked are interchangeable.
- **Integration**: a `make cook` / CMake target (and/or a control command `material.bakeAll` the editor can
  trigger before packaging) that walks the catalog and produces the bundle. Re-bake only stale entries
  (hash changed).

## Files to touch

- `engine/source/saffron/` (material compiler) — a `bakeMaterialShaders(project) -> bundle` that compiles all
  graphs to SPIR-V keyed by hash; reuse the phase-18 emitter.
- `engine/source/saffron/rendering/renderer_pipelines.cpp` — the runtime load path prefers baked SPIR-V by
  graph hash; the runtime-compile path is editor-only (compile-time gated).
- `Makefile` / `cmake/` — a `cook`/`bake` target; (optional) a `material.bakeAll` control command.
- `tools/ci/check.sh` — (optional) include a bake smoke for a sample material project.

## Steps

1. `bakeMaterialShaders`: walk the catalog, compile each graph's permutations to SPIR-V, write the bundle
   keyed by graph hash + permutation; skip unchanged hashes.
2. Runtime: load baked SPIR-V by hash; gate the runtime-`slangc` path behind the editor flag.
3. A `make cook` target + (optional) `material.bakeAll` command; bake a sample project.
4. Verify: with the editor compiler disabled, a baked project renders all materials (no `slangc` invoked).

## Gate / done

- `make engine` clean; a cooked project renders all node-graph materials with the runtime compiler disabled
  (assert `slangc` is never spawned at runtime).
- Re-bake skips unchanged graphs. `make prepare-for-commit` clean. Docs: the cook/bake pipeline.

## Risks

- **Permutation coverage**: the cook must bake every permutation the runtime can request (static/skinned ×
  passes) or a shipped build hits a missing-shader path. Enumerate from the same key space as the runtime cache.
- **Bundle staleness**: key strictly on graph content hash; a mismatch silently loads the wrong shader.
  Make the runtime assert the loaded hash matches the requested one.
- **Determinism**: `slangc` output should be stable for a given input; pin the Slang version (the toolbox
  pins it) so cooked hashes are reproducible.
