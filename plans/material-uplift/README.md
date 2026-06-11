# Material uplift

**Status:** IN PROGRESS (phases 01–05 complete)

A native, editable material system for SaffronEngine: import full PBR texture sets,
manage materials as first-class assets, assign them to entities, edit them in a
sophisticated UI with a live preview sphere, and — the endgame — author them in a
real Unreal-style **node graph** that generates per-material shader code.

This plan is grounded in the engine as it exists on `main` (verified June 2026). It
is deliberately fine-grained: each numbered phase is an independently landable unit
that ends green (`make engine` + `make prepare-for-commit`) and leaves a usable tree.

## Why, in one paragraph

Today a "material" is two inline texture slots (albedo + glTF metallic-roughness) plus
scalar factors, baked straight into the per-entity `MaterialComponent`/`MaterialSetComponent`
and the per-instance buffer. There is no material *asset*, no normal/AO/height support, no
importer for downloaded PBR sets, no editor for materials, and no way to share one material
across entities. Dropping a real scanned material (e.g. Poly Haven `coast_sand_rocks_02`:
diffuse + normal-GL + roughness + 16-bit displacement) renders flat. This plan closes that
gap and ends at a node-graph material editor.

## The five load-bearing decisions

1. **Keep the lighting übershader; carve out a surface seam.** `mesh.slang`'s `fragmentMain`
   is already two halves: a ~15-line *surface* section (sample albedo/MR, set `albedo/metallic/roughness/n`)
   and a ~120-line *lighting* section (clustered + IBL + probes + SSAO + SSGI + DDGI + ReSTIR +
   shadows). We extract the surface half into `SurfaceData evalSurface(MaterialInput)` and leave
   the lighting half untouched and shared **forever**. We do **not** migrate away from the
   übershader — a dynamic-branching übershader for *lighting* is correct. The node graph only
   ever authors `evalSurface`; lighting is never regenerated per material. This keeps PSO count
   linear in material count instead of combinatorial.

2. **Per-material data lives in a `MaterialParams` SSBO, not crammed into the instance.** The CPU
   `InstanceData` (192 B) has a few free lanes (`texture.w`, `pbr.zw`, `emissive.w`), but a full
   PBR set needs more, and node-graph materials have *arbitrary* parameter counts that can never
   fit a fixed instance struct. So `InstanceData` carries a single `uint materialIndex` (in
   `texture.w`), and a new `StructuredBuffer<MaterialParams>` at **set 2, binding 2** holds all
   texture indices + scalars. The draw-list dedups resolved materials into that table — which is
   exactly the edit-once-propagate behaviour shared material assets need anyway.

3. **`.smat` is a reference-only property bag (Unity `.mat`, not UE `.uasset`).** It bakes nothing:
   übershader/permutation selector + texture references by catalog `Uuid` + a flat factor block +
   a feature set. Color space and normal convention are normalized once at **texture import** and
   recorded on the `AssetEntry`; the `.smat` trusts them. Many entities share one `.smat`.

4. **Import = an auto-detecting material importer (default) + manual "Make Material" (fallback).**
   Drag a folder/zip → suffix-detect each map's role/colorspace/channel → propose a `.smat` with
   confidence → user confirms. Never silently commit ambiguous guesses (colorspace/normal mistakes
   are unrecoverable). The manual path imports textures then opens an empty material.

5. **Preview = an in-frame render of the real übershader on a sphere, lit by a baked studio IBL.**
   The existing `thumbnail.spv` pipeline binds *zero* descriptor sets and cannot render PBR, so it
   is reused only for the readback→PNG tail. The preview renders the **actual `mesh.slang`** into a
   small offscreen target with a synthetic globals UBO (one key light, IBL on, all screen-effect
   flags off so DDGI/SSGI/ReSTIR/shadow sets just need dummy bindings) and a dedicated studio IBL
   baked once at startup — so the preview is WYSIWYG. Delivered to the editor as a PNG/blob over the
   control plane (no second Wayland subsurface — that is blocked behind the not-started dmabuf work).

### The node graph, concretely

The graph is a **frontend that generates the body of `evalSurface`**. Its terminal pins are the
übershader's surface inputs (Base Color, Normal, Metallic, Roughness, Emissive, Occlusion, Height,
Opacity). Backend: graph → Slang source → `slangc` → SPIR-V → PSO, keyed by graph hash, **linked
against the shared lighting compiled once as a Slang module** (Slang's interfaces/generics/
separate-compilation are why touching lighting won't recompile every material — the trap UE hit).
Runtime shader compilation is an **editor-only** concern; shipped builds load **cook-time-baked**
SPIR-V, so the "no runtime compiler" property is preserved. The editor view is built on **React Flow
(xyflow)**. A graph that only folds to parameters needs no codegen (phase 17); the real codegen
backend (phase 18) is where triplanar/noise/custom-Slang become possible.

## Phases

| # | Phase | Goal | Gate / unblocks | Depends on |
|---|-------|------|-----------------|------------|
| 01 | [Surface/lighting seam](phase-01-surface-lighting-seam.md) | Extract `evalSurface` from `fragmentMain`, define the `SurfaceData`/`MaterialInput` ABI; zero behaviour change | The seam the node graph and feature bits plug into exists | — |
| 02 | [Material params buffer](phase-02-material-params-buffer.md) | `MaterialParams` SSBO (set 2 binding 2) + `Instance.materialIndex` + draw-list material dedup | Per-material data scales to arbitrary params; instance shrinks | 01 |
| 03 | [Native `.smat` asset](phase-03-native-smat-asset.md) | `AssetType::Material`, `.smat` JSON format, load/save, built-in default material | Materials are first-class catalog assets | 02 |
| 04 | [Tangents + `.smesh` v3](phase-04-tangents-vertex-format.md) | Add tangent vertex attribute, bake/import tangents, plumb to shader | Tangent-space normal mapping is possible | — |
| 05 | [PBR texture slots](phase-05-pbr-texture-slots.md) | normal + packed ORM (+channel routing) + emissive tex + feature bits + UV tiling/offset in `MaterialParams`/`evalSurface` | A `.smat` expresses a real PBR surface | 02, 04 |
| 06 | [Parallax + alpha + double-sided](phase-06-parallax-alpha-doublesided.md) | Height/parallax-occlusion in `evalSurface`, alpha-clip (masked), `doubleSided` PSO axis | Displacement-style depth + cutout foliage | 05 |
| 07 | [Texture import upgrades](phase-07-texture-import-upgrades.md) | EXR (tinyexr) + 16-bit PNG decode + colorspace/normal-convention metadata + DX→GL & gloss→rough bake | Real downloaded PBR sets decode correctly | 03 |
| 08 | [Material importer (auto-detect)](phase-08-material-importer.md) | Suffix→role/colorspace/channel table, folder/zip drop, proposal-with-confidence, `material.import` | Drag-a-folder → confirmed material in seconds | 05, 07 |
| 09 | [Entity material assignment](phase-09-entity-material-assignment.md) | `MaterialAssetComponent` + set-slot handle + resolve precedence + serde regen | Entities reference shared material assets | 03 |
| 10 | [Material CRUD commands](phase-10-material-crud-commands.md) | `material.create/list/get/update/assign` DTOs + gen.ts + `se` verbs + e2e | Materials are scriptable/testable from the shell | 03, 09 |
| 11 | [Studio IBL + preview pass](phase-11-studio-ibl-preview-pass.md) | Ship a studio HDR, bake a dedicated preview IBL, in-frame preview render pass of the real übershader | Engine can produce a WYSIWYG material preview image | 05 |
| 12 | [Preview command + thumbnails](phase-12-preview-command-thumbnails.md) | `preview.render`/`preview.configure` (PNG blob) + cached material thumbnails | Editor can request/show preview images | 11 |
| 13 | [Material Editor panel](phase-13-material-editor-panel.md) | `RightTool: "material"` slot-inspector over the field renderer + catalog plumbing + live preview sphere | Artists edit materials visually with instant feedback | 10, 12 |
| 14 | [Entity material picker](phase-14-entity-material-picker.md) | Inspector `Material.material` → `AssetPicker("material")` + assign wiring + default-material display | Assign a material to an entity from the inspector | 09, 13 |
| 15 | [Engine hardening](phase-15-engine-hardening.md) | Bindless slot reclamation (free-list), mipmaps + BC compression, dangling-reference validation | Production texture sets don't exhaust the pool or alias | 05 |
| 16 | [Material instances/variants](phase-16-material-instances.md) | `.smat` `parent` + sparse `overrides`, parent→child resolve, override UI | One master material drives many surfaces | 13 |
| 17 | [Node-graph data model](phase-17-node-graph-data-model.md) | Graph schema (nodes/pins/edges), `.smatg`/embedded storage, DTOs, **params-only** lowering | A graph round-trips and drives a material via parameters | 10 |
| 18 | [Slang codegen backend](phase-18-slang-codegen-backend.md) | Graph → `evalSurface` Slang, host `slangc`, async compile + fallback, PSO cache by graph hash, module linking | A graph generates real per-material surface code | 01, 17 |
| 19 | [Node library](phase-19-node-library.md) | Typed Slang-snippet nodes: texture/UV/math/vector/normal-blend/triplanar/noise/Fresnel/time/vertex-color/panner/custom | Artists author non-trivial procedural materials | 18 |
| 20 | [React Flow editor view](phase-20-react-flow-editor-view.md) | `ViewTab: material:<uuid>` graph editor (xyflow): palette, typed wiring, per-node previews, live result | The UE5-style live node graph, end to end | 19 |
| 21 | [Cook-time shader baking](phase-21-cook-time-shader-baking.md) | Bake every material graph's SPIR-V into the asset at build; runtime loads precompiled | Shipped builds need no runtime compiler | 18 |

Rough milestone reading: **01–10** is a complete native material system with import and
scripting (no UI graph); **11–14** adds the visual editor with live preview; **15–16** hardens
and adds instances; **17–21** is the full node-graph endgame. Each block is independently valuable.

## Cross-cutting conventions (apply to every phase)

- **Milestone gate per phase:** finish with `make engine` then `make prepare-for-commit` green;
  fix every warning your change raises. Per AGENTS.md, this is part of "done", not a final step.
- **A feature that adds engine state gets an `se` command** (one `registerCommand` in `Saffron.Control`)
  so the running editor stays scriptable and visually debuggable from a shell.
- **Generated code is regenerated, never hand-edited:** `scene_component_serde.generated.cpp` and
  `control_dto_serde.generated.cpp` come from `tools/gen-control-dto/gen.ts` (`bun run` it). Adding a
  component or command means editing `gen.ts`'s catalog + the C++ struct, then regenerating.
- **Docs:** a phase that adds/alters an engine concept updates the matching page under `docs/content/`
  and its hub `_index.md` row in the same change (use the `docs-page` skill).
- **Status line:** flip this README's row and each phase file's `**Status:**` to `IN PROGRESS`/`COMPLETED`
  as work lands. Delete a phase file only after it is `COMPLETED` and merged.

## Key verified anchors (June 2026)

Renderer:
- `InstanceData` (192 B) — `renderer_types.cppm` (`texture.x`=albedo, `.y`=jointOffset, `.z`=mr, `.w`=FREE; `pbr.xy`=metallic,roughness, `.zw`=FREE; `emissive.rgb`, `.w`=FREE).
- `SubmeshMaterial`, `DrawItem`, `Material{shader,unlit}` — `renderer_types.cppm`.
- Shader `Instance` + `fragmentMain` surface(≈L466–480)/lighting(≈L481–602) split — `engine/assets/shaders/mesh.slang`.
- `submitDrawList` builds the instance buffer (192 B stride) — `renderer_drawlist.cpp`.
- `requestMeshPipeline` — string cache key `shader[+"|unlit"][+"|skinned"]`; sets 0–5 (+6/7 RT/ReSTIR); `constant_id(0)=kUnlit`; blend/cull/formats hardcoded — `renderer_pipelines.cpp`.
- `bakeEnvironment(renderer, params, first)` + `Ibl{irradianceCube,prefilteredCube,brdfLut,setLayout(set 3),set}` — `renderer_detail.cppm` / `renderer_types.cppm`.
- `renderMeshThumbnail` (flat, **no descriptor sets**), `encodeTextureThumbnailPng`, `captureImageToBuffer`, `captureViewport` — `renderer_thumbnail.cpp` / `renderer_capture.cpp`.
- Render-graph pass pattern: `RgPass{name,kind,color,depth,renderArea,accesses,execute}` + `addPass` in `beginFrameGraph` — `renderer.cppm`.

Assets / scene:
- `AssetType{Mesh,Texture,Other,Animation}` + `AssetEntry{id,name,type,path,folder,hdr,linear,duration}` + `AssetCatalog{entries,folders,byId}` — `scene.cppm`; `assetTypeName`/`assetTypeFromName`, `catalogToJson`/`FromJson` — `assets.cppm`.
- `AssetServer{root,catalog,meshRefByUuid,textureRefByUuid}`; `loadMeshAsset`, `loadTextureAsset`, `registerTextureBytes(srgb)`, `registerHdrTextureBytes`, `importModel` — `assets.cppm`.
- `uploadTexture` (`eR8G8B8A8Srgb` vs `Unorm` by `srgb`), `uploadTextureFloat` (`eR16G16B16A16Sfloat`), `nextBindlessIndex` (no reclamation), `MaxBindlessTextures=1024` — `renderer_textures.cpp` / `renderer_types.cppm`.
- `decodeImage`/`decodeImageFromMemory`/`decodeImageHdr` (stb only, no EXR, no `stbi_load_16`), `ImportedMaterial`, `extractGltfMaterial`, `SMeshHeader` (v1/v2), `saveMesh`/`loadMesh` — `geometry.cppm`.
- `resolveEntityMaterials` → `SubmeshMaterial` (the `lower` lambda) — `assets.cppm`.
- `MeshComponent`, `MaterialComponent`, `MaterialSlot`, `MaterialSetComponent` (all texture refs are `Uuid`, 0=none) — `scene.cppm`.

Control:
- `registerCommand<Params,ResultDto>(reg,name,help,handler)` — `command.cppm`; registered in `control_commands_{render,scene,asset,animation}.cpp`.
- DTOs incl. `AssetSlotDto{Mesh,Albedo,MetallicRoughness}`, `AssignAssetParams`, `SetMaterialParams` — `control_dto.cppm`.
- `gen.ts`: `commands[]`, `commandFixtures`, `commandSkips`, `emitSceneSerde()`; outputs `*_serde.generated.cpp`, `editor/src/protocol/se-types.ts`, `openrpc.generated.json`, `command-manifest.generated.json`.
- `se` CLI auto-forwards any registered command — `tools/se/source/main.cpp`.

Editor:
- `useEditorStore`; `RightTool="stats"|"profiler"`; `ViewTab` union (scene/flamegraph/asset); `assets`, `refreshAssets`, `selectedId`, `selectionVersion`, `sceneVersion`, `dragActive` — `editor/src/state/store.ts`.
- `client.{setMaterial,assignAsset,listAssets,setComponentField,getThumbnail}` + `call<C>` — `editor/src/control/client.ts`.
- `FIELD_HINTS`, `renderField`, `AssetKind="mesh"|"texture"` — `editor/src/components/fieldRenderer.tsx`; `AssetPicker` filters `a.type===assetType` — `AssetPicker.tsx`.
- `getThumbnailUrl`/`base64ToBlob` + drag-drop import — `AssetsPanel.tsx`; `makeCoalescer` — `coalesce.ts`; right-tool wiring — `RightSidebar.tsx`; view dispatch — `App.tsx`.
