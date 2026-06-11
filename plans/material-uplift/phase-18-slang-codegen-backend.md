# Phase 18 — Slang codegen backend

**Status:** IN PROGRESS — codegen **renders** in the preview (graph → Slang → slangc → PSO → image); scene-path codegen + caching remain
**Depends on:** 01, 17

> **Render-wiring done (preview path).** `compileMaterialPreviewShader` emits a full preview shader
> (matching `PreviewPush` + the sphere vertex layout) whose `evalSurface` is the graph (via
> `emitGraphSurface`), slangc-compiles it to `materials/<id>_preview.spv`. `newPreviewPipeline` is now
> parameterized by spv path; `renderMaterialPreview` takes an optional `shaderSpv` (a per-call codegen
> pipeline). `preview-render` detects a **non-foldable** graph → codegens → renders the procedural surface.
> e2e `material_codegen_render.test.ts` proves a multiply-graph material **renders** via codegen
> (valid PNG, validation-clean). **The headline graph → Slang → slangc → PSO → rendered-image pipeline is
> complete for the preview.** Remaining: (1) **scene-path codegen** (the übershader for actual entities,
> not just the preview — splice the emitted `evalSurface` into a per-material mesh PSO via
> `requestMeshPipeline` keyed on graph hash); (2) **per-graph pipeline caching** (currently per-call) +
> editor async-compile/fallback; (3) render-context nodes (triplanar/noise/Fresnel/normal). Phases 20
> (React Flow editor) + 21 (cook) follow.

> **Done (codegen core).** `emitGraphSurface(graph)` lowers a node graph to a Slang `evalSurface` body
> (constant / textureSlot / multiply / add nodes → statements in array order, then the `materialOutput`
> channel assignments). `findSlangc()` locates the compiler (env `SAFFRON_SLANGC` → the prebuilt cache →
> PATH). `compileMaterialGraph` splices the emitted body into a self-contained shader and shells `slangc`
> → `materials/<uuid>.spv`. `material-compile-graph {material}` runs it. e2e `material_codegen.test.ts`
> proves a **non-foldable multiply graph** is detected (`foldable=false`) and **codegens to compilable
> SPIR-V** (`ok=true`). This establishes the headline graph→shader pipeline end to end (feasibility was the
> big unknown — slangc is locatable + runs from the host).
>
> **Remaining for full phase 18:** wire the compiled `.spv` into a **per-material PSO** + the render path
> — splice the emitted `evalSurface` into the real shader (übershader for scene, or `preview.slang` for the
> preview) rather than the standalone validation shell, key the PSO cache on the graph hash, set
> `Material.shader` to the codegen'd module in `resolveEntityMaterials`, and (editor) async-compile +
> fallback. Then the codegen material actually *renders* with its custom surface. (Phases 19 node library,
> 20 React Flow editor, 21 cook-time baking follow.)

## Goal

The real node-graph backend: lower a material graph to a Slang **`evalSurface`** body, compile it with
`slangc`, and load the result as a per-material PSO — **linked against the shared lighting compiled once as
a Slang module**. Async compile with a fallback material while building; PSO cache keyed by graph hash.
This is what makes triplanar/noise/custom math possible (phase 19's nodes), and it does **not** touch the
lighting code — the seam from phase 01 is the entire integration surface.

## Why

A params-only graph (phase 17) can't do procedural surface math. Codegen of `evalSurface` is the UE
material model done right for this engine: one PSO per unique graph (× static/skinned × passes), lighting
shared, so PSO count stays linear in material count. Slang's interfaces/generics/separate-compilation are
why touching lighting won't recompile every material (the trap UE hit with HLSL stitching).

## Design

- **Lowering**: topologically sort the graph; emit a Slang snippet per node (phase 19 supplies the snippets)
  into a generated `evalSurface(MaterialInput) -> SurfaceData` body; uniform inputs (constants, texture
  indices) come from the `MaterialParams` buffer (so editing a constant is a buffer write, **not** a recompile —
  only graph *topology* changes trigger compile).
- **Module linking**: compile the shared lighting (the phase-01 `fragmentMain` lighting half + BRDF + IBL +
  GI) **once** as a Slang module exposing `evalLighting(SurfaceData, …)`. Each material compiles only its
  generated `evalSurface` + the thin entry point, linking the lighting module. `slangc` separate compilation /
  link-time specialization makes this incremental.
- **Invocation**: the host shells out to `slangc` (the toolbox has it) or embeds the Slang API. Editor-only:
  async on a worker, with status (compiling/ok/error) surfaced over the control plane; show the fallback
  material (default/last-good) while compiling; report compile errors to the editor (node-attributed if possible).
- **PSO cache**: key = graph content hash (+ skinned + pass). Extend `requestMeshPipeline`'s string key with
  the graph hash; cache the compiled SPIR-V on disk under the project so re-opens are instant.

## Files to touch

- `engine/assets/shaders/` — split `mesh.slang` so the lighting half is a reusable Slang module; the
  generated material shader `import`s it and supplies `evalSurface`.
- `engine/source/saffron/` (a new `Saffron.MaterialCompiler` area, or under host/rendering) — graph→Slang
  emitter, `slangc` invocation, async compile queue, compiled-SPIR-V cache, compile-status reporting.
- `engine/source/saffron/rendering/renderer_pipelines.cpp` — graph-hash in the PSO cache key; load the
  generated SPIR-V; fallback pipeline while compiling.
- `engine/source/saffron/control/` — `material.compileStatus` / push compile errors to the editor.

## Steps

1. Refactor `mesh.slang` into `lighting` (module) + a thin entry that calls `evalSurface`+`evalLighting`;
   verify identical output (a regression vs phase 01).
2. Build the graph→Slang emitter for a minimal node set (texture, constant, multiply, material output);
   compile via `slangc`; load the SPIR-V; render a sphere.
3. Async compile + fallback-while-building + error reporting; on-disk SPIR-V cache keyed by graph hash.
4. PSO cache key extension; verify N graphs → N PSOs (not combinatorial), lighting compiled once.
5. e2e: a procedural graph (e.g. checker via math nodes) compiles and renders; editing a constant does
   **not** recompile (buffer write); editing topology does.

## Gate / done

- `make engine` clean; the refactored `mesh.slang` renders identically; a codegen graph compiles + renders.
- Constant edits = no recompile; topology edits recompile async with a visible fallback.
- `make prepare-for-commit` clean. Docs: the codegen pipeline + the param-vs-topology cost model.

## Scene-path codegen — concrete plan (the remaining chunk)

The preview path renders codegen materials on the sphere. To render them on **scene entities** the
generated `evalSurface` must go into the real übershader (`mesh.slang`) as a per-material PSO. Worked-out
approach (grounded in the current code):

1. **Make the emitter context-aware.** `emitGraphSurface` currently hardcodes the *preview* context:
   `mat.baseColor`, `mat.tex.x`, the array name `textures`, a default `s.normal = float3(0,0,1)`, and it
   does **not** set `s.occlusion`/`s.opacity` (the preview `SurfaceData` lacks them; mesh.slang's has
   them). Thread an `EmitContext { texArray, baseColorExpr, texIndexExpr(slot), uvExpr, normalDefault,
   extraFixups }` through it. Preview ctx = today's strings; mesh ctx = `albedoTextures`,
   `m.mat.baseColor`, `m.mat.tex0/tex1` per slot, `m.uv0 * m.mat.uv.xy + m.mat.uv.zw`, and a fixup tail
   `s.normal = normalize(m.worldNormal); s.occlusion = 1.0; s.opacity = base.a …`. Note `textureSlot`
   ignores `props.slot` today (always albedo) — make it honor the slot via `texIndexExpr`.
2. **Splice into mesh.slang.** Copy the `*.slang` sources next to the exe in CMake (today only the `.spv`
   ships). Mark `evalSurface`'s body in `mesh.slang` with `// @graph-begin`/`@graph-end`. At compile time,
   read the runtime `mesh.slang`, replace the marked region with the mesh-context emit, write a temp, run
   `slangc` → `materials/<hash>_mesh.spv`. (mesh.slang is one self-contained 750-line file, no includes —
   so this is a pure text splice.)
3. **Per-material mesh PSO.** When `loadMaterialAsset` sees a **non-foldable** graph, compile the mesh
   variant and set `MaterialAsset`/the resolved `Material.shader` to the absolute `_mesh.spv` path.
   `requestMeshPipeline` already keys on the shader string, so a per-graph PSO falls out; teach
   `newMeshPipeline`/`loadShaderModule` to accept an absolute path (today it's `assetPath`-relative).
   Cache the compiled spv on disk by graph hash so re-opens are instant; async-compile with the default
   übershader as the fallback while building.
4. **e2e.** Assign a codegen material to an entity, screenshot, assert the procedural surface renders and
   the log is validation-clean — and that a *foldable* graph still draws on the shared übershader (PSO
   count doesn't grow per material).

This is a deep integration (scene draw → `Material.shader` → PSO → `newMeshPipeline`), higher-risk than
the preview path; do it as its own focused pass, gating on the existing preview e2e after the emitter
refactor so that change is proven safe before the splice.

## Risks

- **"Recompile the world"**: if the lighting isn't a properly linked module, every material recompiles when
  lighting changes. Get Slang module/`import` linking right — this is the make-or-break of the whole endgame.
- **Compile latency / UX**: shelling `slangc` per edit is slow if naive; debounce on topology change only,
  cache aggressively, keep the fallback visible. Never block the present loop on a compile.
- **Runtime compiler footprint**: this is **editor-only**. Shipped builds must not need `slangc` — phase 21
  bakes SPIR-V at cook time. Keep the runtime-compile path behind an editor flag.
- **Error attribution**: map `slangc` errors back to nodes where feasible; at minimum surface the message.
