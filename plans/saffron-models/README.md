# Saffron models

**Status:** COMPLETED — bake each import into one self-contained `.smodel` container; derive the asset catalog from a filesystem scan; decouple import from instantiation; support sub-asset extraction, diffing reimport, and deliberate cleanup.

A model-asset system for SaffronEngine. Importing a multi-mesh / multi-texture scene (e.g. Sponza,
~50 textures) today scatters ~50 loose files into the catalog and immediately spawns an entity. This
plan replaces that with **one self-contained `.smodel` container** per import — a header-first binary
that bakes the meshes, materials, textures, animations, and the node/transform hierarchy into a single
file — and makes the **filesystem the source of truth** for the catalog, so an unsaved import can never
become a dead orphan. Import produces an *asset*; you **instantiate it on demand** (drag into the scene)
as many times as you like, and you can **extract** any embedded sub-asset to a standalone file when you
want to edit or share it.

This plan is grounded in the engine as it exists on `main` (verified June 2026). It is deliberately
fine-grained: each numbered phase is an independently landable unit that ends green (`make engine` +
`make prepare-for-commit`) and leaves a usable tree. Migration of existing projects is **out of scope**
(start a fresh project) — every phase is written for the clean-slate path.

The design borrows the best of three pipelines: **UE5** (Interchange's read→decide→build split; the
Asset Registry's header-first, scan-derived, regenerable index; deterministic reimport from a stored
recipe), **Unity** (the Model-Prefab container with meshes/materials/textures as sub-assets; embedded
by default, extract-on-demand with a remap table that survives reimport; `.meta` identity; filesystem
as truth), and **glTF/GLB** (a fixed header + a chunk TOC + payload views, with a separate node-graph
block). We take their shapes and drop their footguns (UE's opaque `.uasset`, GLB's base64/4 GiB cap,
Unity's lost-`.meta`).

## Why, in one paragraph

Today `importModel` (`assets.cppm:2128`) parses → bakes a `.smesh` → uploads → registers textures →
adds catalog rows in one call, and the `import-model` command (`control_commands_asset.cpp:522`)
immediately `spawnModel`s an entity. The artifacts are **separate files**: one `models/<uuid>.smesh`
(geometry only), N `textures/<uuid>.<ext>`, optional `.sanim` clips, and — critically — **no `.smat`
at all**: imported materials are never written to disk, they live as inline components on the spawned
entity (`applyImportedMaterials`, `assets.cppm:2329`). The catalog is **hand-authored in `project.json`**
(`catalogToJson`/`catalogFromJson`, `assets.cppm:289`/`338`); there is no filesystem scan, so a dead
`mesh` uuid renders nothing silently and an import you forget to save leaves orphaned files on disk that
the catalog never knew about. The node hierarchy isn't persisted (`ImportedNode`/`ImportedSkin` die after
spawn). This plan closes all four gaps: one durable container, a scan-derived catalog, decoupled
instantiation, and first-class embedded materials.

## The load-bearing decisions

1. **One self-contained `.smodel` container, header-first.** A fixed `SMDL` `FileHeader` + a `ChunkTOC` +
   a JSON `MetadataChunk` (front-loaded) + payload chunks. A *prefix read* (header + TOC + metadata) is
   enough to build a catalog entry without ever touching geometry/texture bytes — the UE Asset Registry
   model. `.smodel` is a **new magic/version**; `.smesh` stays geometry-only and is reused verbatim as a
   chunk payload (no `.smesh` v3).
2. **Imported materials are always embedded as `.smat`-JSON chunks** (reusing the completed material-uplift
   format), each with a stable sub-id — so they are reusable, extractable, and overridable. The
   MetadataChunk also carries a per-material factor summary for prefix-read display. This closes the
   "imported materials are ephemeral" gap.
3. **Filesystem is the source of truth.** The catalog is rebuilt by **scanning `assets/`** and prefix-reading
   every `.smodel`. Identity is a stable 64-bit `Uuid` stored in the header / MetadataChunk (containers) or
   a `.smeta` sidecar (foreign/headerless files). **All references are by `Uuid`, never by path or array
   index.** `project.json` keeps only `assetFolders` + UI state. A regenerable `catalog.json` cache is a
   latency nicety, never load-bearing (deleting it is always safe).
4. **UUID filenames, identity in the bytes.** Engine-written files keep the existing convention
   (`models/<uuid>.smodel`, extracted `materials/<uuid>.smat`, `textures/<uuid>.<ext>`). No rename
   collisions; no lost-`.meta` footgun for engine files (identity lives in the header).
5. **Decoupled instantiation.** Import produces the `.smodel` + catalog rows and **does not spawn**.
   `instantiateModel` expands the stored node hierarchy into entt entities on demand (one asset → many
   instances → or never placed), holding soft `(modelId, subId)` references resolved at render time.
6. **Extract + remap.** `extractSubAsset` slices a chunk to a standalone file **keeping its sub-id**, and
   writes a remap record into the container so resolution prefers the external file. Reimport never clobbers
   a remapped (extracted/edited) sub-asset — the Unity rule.
7. **Deliberate cleanup, never automatic.** Cleanup walks reachability from protected roots (the active
   scene), classifies candidates as unused / orphaned-file / broken-reference / indirect-review, and deletes
   only on explicit confirm. `.smodel` supersedes the `plans/editor-view` `.srig` sidecar for rig
   persistence (the MetadataChunk's `nodes`+`skin` is exactly that payload).

## Phases

| # | Phase | Goal | Gate / unblocks | Depends on |
|---|-------|------|-----------------|------------|
| 01 | [The `.smodel` container format](phase-01-smodel-container-format.md) | `SMDL` `FileHeader` + `ChunkTOC` + fourcc chunks + low-level read/write in `Saffron.Geometry`, payloads opaque; round-trip self-test | The byte container every later phase writes/reads exists | — |
| 02 | [MetadataChunk + prefix-read](phase-02-metadata-chunk-prefix-read.md) | JSON MetadataChunk schema (model/import/subAssets/materials/nodes/skin/remap) + cheap `readContainerMetadata` prefix read | Catalog-scannable metadata without touching payloads | 01 |
| 03 | [`AssetType::Model` + sub-asset rows](phase-03-asset-type-model-catalog-rows.md) | `AssetType::Model`; `AssetEntry` container linkage (container id + chunk); serde regen | The catalog can express a model + its sub-assets | 02 |
| 04 | [Import-graph translator + `ImportOptions`](phase-04-import-graph-translator.md) | `importModelWithMaterial` → format-neutral graph w/ deterministic stable sub-ids; serializable `ImportOptions` | The read→decide split; options own all decisions | 03 |
| 05 | [Container writer (bake `.smodel`)](phase-05-container-writer-bake.md) | `bakeModel`: graph+options → per-sub-asset byte images (incl. `.smat` chunks) → one `.smodel` + catalog rows; no GPU/spawn | Imports produce one self-contained file | 04 |
| 06 | [Chunk-slice loaders + GPU cache by sub-id](phase-06-chunk-slice-loaders.md) | `loadModelAsset`: resolve sub-id → chunk slice → load mesh/tex/anim/mat via the negative-cache; honor remap | GPU resources load from inside a container | 05 |
| 07 | [Decoupled instantiate](phase-07-decoupled-instantiate.md) | `instantiateModel`: expand MetadataChunk hierarchy → entt via `spawnModel`/`spawnSkinnedModel`; import stops spawning | One asset → many instances | 06 |
| 08 | [Split import/instantiate commands](phase-08-import-instantiate-commands.md) | `import-model-to-asset` (no spawn) + `instantiate-model`; DTOs + gen + `se` + contract fixtures | The decoupled flow is scriptable | 07 |
| 09 | [Filesystem scan + catalog rebuild](phase-09-filesystem-scan-catalog.md) | `scanAssets`: walk `assets/`, prefix-read containers, diff catalog, patch GPU caches; `loadProject` builds from scan; `scan-assets` cmd | Filesystem becomes source of truth; orphans impossible | 08 |
| 10 | [`.smeta` sidecar + colorspace](phase-10-smeta-sidecar-colorspace.md) | `.smeta` for foreign/headerless files; colorspace from chunk flags / sidecar, not only `AssetEntry.linear` | Dropped raw textures get stable identity + colorspace | 09 |
| 11 | [Catalog cache (regenerable)](phase-11-catalog-cache.md) | `assets/.cache/catalog.json` keyed by path→(mtime, hash, subIds); rebuild on miss; never load-bearing | Cold-start latency without changing truth | 10 |
| 12 | [Sub-asset extraction + remap](phase-12-subasset-extraction-remap.md) | `extractSubAsset`: slice → standalone file keeping sub-id + remap record; `extract-subasset` cmd | Edit/share any embedded sub-asset | 11 |
| 13 | [Diffing reimport preserving overrides](phase-13-diffing-reimport.md) | Skip-if-unchanged (hash); else re-bake with stored options, diff by sub-id, never clobber remapped externals; `reimport-model` | Source edits flow through; extracted edits survive | 12 |
| 14 | [Reference graph + inspector](phase-14-reference-graph-inspector.md) | Dependency graph + `model-info` / `asset-references` (what-references-this / footprint rollup) | Reference Viewer / Size Map analog | 13 |
| 15 | [Clean/orphan tooling](phase-15-clean-orphan-tooling.md) | Reachability-from-roots classify (unused/orphaned/broken/indirect) + `clean-assets` (dry-run) + `delete-unused` (confirm) | Deliberate, safe cleanup | 14 |
| 16 | [Editor: model tiles, drag-instantiate, extract, clean](phase-16-editor-model-tiles.md) | Hierarchical asset tiles, drag-model→`instantiate-model`, context Extract, clean review modal | The whole flow is usable in the editor | 15 |
| 17 | [Thumbnails baked at import + async](phase-17-thumbnails-baked-import.md) | Bake container + per-sub-asset previews at import; async off the frame loop; key by `(modelId, subId)` | One-item browser shows previews without stalls | 16 |
| 18 | [Docs, e2e hardening, gate](phase-18-docs-e2e-gate.md) | `.smodel` docs page + cleanup how-to + hub rows; full-flow e2e; `make check` green; humanizer | The plan is documented, tested, and complete | 17 |

Rough milestone reading: **01–09** is the container + scan foundation (imports bake into one file, the
catalog is scan-derived, instantiation is decoupled, all scriptable); **10–13** adds durability, sidecar
identity, extraction, and diffing reimport; **14–15** adds the reference graph and deliberate cleanup;
**16–18** surfaces it in the editor, bakes thumbnails, and lands docs + tests. Each block is independently
valuable.

## Cross-cutting conventions (apply to every phase)

- **Milestone gate per phase:** finish with `make engine` then `make prepare-for-commit` green; fix every
  warning your change raises. Per AGENTS.md this is part of "done", not a final step. Wire phases also run
  `make e2e` + the control-schema contract test; editor phases also `bun run check` + `bun run lint`.
- **A feature that adds engine state gets an `se` command** (one `registerCommand` in `Saffron.Control`) so
  the running editor stays scriptable and debuggable from a shell.
- **Generated code is regenerated, never hand-edited:** `scene_component_serde.generated.cpp`,
  `control_dto_serde.generated.cpp`, `editor/src/protocol/se-types.ts`, `openrpc.generated.json`,
  `command-manifest.generated.json` all come from `tools/gen-control-dto/gen.ts` (`bun run` it). The contract
  test checks raw bytes (uuids are decimal strings, not numbers).
- **Docs:** a phase that adds/alters an engine concept updates the matching page under `docs/content/` and
  its hub `_index.md` row in the same change (use the `docs-page` skill).
- **Status line:** flip this README's row and each phase file's `**Status:**` to `IN PROGRESS`/`COMPLETED`
  as work lands. Delete a phase file only after it is `COMPLETED` and merged.
- **Hard constraints:** never bump `ProjectVersion` (`assets.cppm:681`, hard-locks old builds); never bump
  `.smesh` to v3 (`loadMesh` hard-fails on unknown versions, `geometry.cppm:1190`) — `.smodel` is a new
  format; **git is read-only** — leave work unstaged.

## Key verified anchors (June 2026)

Import / assets:
- `importModel` (parse→bake→upload→register, the coupled path) — `assets.cppm:2128`; `ImportResult`/`ImportedModel`/`ImportedNode`/`ImportedSkin` — `assets.cppm:112`.
- `applyImportedMaterials` (imported materials → inline entity components, never `.smat`) — `assets.cppm:2329`.
- `spawnModel` / `spawnSkinnedModel` / `relinkHierarchy` (uuid-keyed parent → handle) — `assets.cppm`.
- `catalogToJson` / `catalogFromJson` — `assets.cppm:289`/`338`; `saveProject` / `loadProject` — `assets.cppm:765`/`933`; `ProjectVersion` — `assets.cppm:681`.
- `AssetServer{root, catalog, meshRefByUuid, textureRefByUuid}` + `loadMeshAsset` / `loadTextureAsset` (negative-cache) / `registerTextureBytes(srgb)` — `assets.cppm:54`.
- `MaterialAsset` + `materialAssetToJson` / `materialAssetFromJson` + `saveMaterialAsset` / `loadMaterialAsset`; `DefaultMaterialId{1}` — `assets.cppm`.

Geometry:
- `SMeshHeader` (magic/version/counts/offsets, v1/v2 skin) + `saveMesh` / `loadMesh` (version gate L1190, layout check L1205) — `geometry.cppm:243`/`1136`/`1170`.
- `SANimHeader` + `saveAnimation` / `loadAnimation` — `geometry.cppm:263`.
- `importModelWithMaterial` (cgltf/tinyobj front-end) — `geometry.cppm:1027`; glTF texture extraction (buffer-view vs URI) — `geometry.cppm:411`; `extractGltfMaterial`, `decodeImage`.

Scene / control / editor:
- `AssetType{Mesh,Texture,Material,Other,Animation}` + `AssetEntry{id,name,type,path,folder,hdr,linear,duration}` + `AssetCatalog{entries,folders,byId}` — `scene.cppm:331`/`340`.
- `registerCommand<Params,ResultDto>` — `command.cppm`; asset commands incl. `import-model` — `control_commands_asset.cpp:522`; DTOs — `control_dto.cppm`; generator — `tools/gen-control-dto/gen.ts`; `se` CLI auto-forwards — `tools/se/source/main.cpp`.
- `renderMeshThumbnail` (flat, no descriptor sets) + `device.waitIdle` on the frame loop — `renderer_thumbnail.cpp`.
- `AssetsPanel.tsx`, `AssetTile.tsx`, `editor/src/state/store.ts` (`assets`, `refreshAssets`, `dragActive`), `client.ts`.
