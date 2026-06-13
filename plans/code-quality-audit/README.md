# Code-quality audit

**Status:** IN PROGRESS — Phases 1–6 substantially complete and gated; the **full e2e suite is green (239/239 across 68 files)**. Remaining items are deliberately deferred (documented per phase below): the `SCENE_COMPONENTS` generator consolidation (skipped by decision), the residual GPU micro-dedup (Phase 3), the editor `store.ts`/`AssetsPanel.tsx` splits + UI-behavioral coupling (Phase 5) — held because they can't be verified without the running editor — and a **pre-existing VMA teardown leak** (see below; not introduced by this cleanup, not an e2e failure).

> **e2e — all green.** Fixed the two failures surfaced earlier (`probe-asset` made container-aware; `skinning` test finds the rig descendant) plus **10 more pre-existing failures** the in-flight rig-on-descendant / auto-empty-project-path / camera-default refactor had outrun (animation-playback, skeleton-overlay, foot-ik, asset-editor[-static], asset-preview, camera) — all stale-test updates that preserve the original assertions; no product bugs found. Added **17 editor unit-test files (363 cases)** + 3 new e2e tests + a C++ `SubscriberList` self-test.

> **Pre-existing VMA teardown leak (flagged, NOT introduced here):** a clean engine exit (`SAFFRON_EXIT_AFTER_FRAMES`, the CI present-only smoke) aborts on `VmaDeviceMemoryBlock::Destroy` "allocations were not freed". It reproduces with no project, and `destroyRenderer`'s freeing is unchanged by this cleanup (the VMA-leak-fix consolidation is faithful; the splits moved create-not-free code), so it is pre-existing. The e2e harness swallows the quit-abort, so `make e2e` is unaffected. Fix needs VMA-stats instrumentation before `vmaDestroyAllocator` to name the unfreed allocation — a separate task.

A repo-wide audit of code quality and cleanliness across nine dimensions: Go-flavored-C++
convention compliance (`CONVENTIONS.md`), comment hygiene (`AGENTS.md`), dead code, coupling,
duplication, oversized files, oversized UI components, oversized functions, and test gaps.
Findings below were produced by a fan-out review (one deep reviewer per code slice), with
dead-code and convention findings adversarially re-verified against the actual source, plus
five repo-wide cross-cutting passes. **214 verified findings** (6 false positives rejected),
plus 52 cross-cutting findings. The allowed exceptions (GLM/`vk::`/nlohmann operators, RAII
wrapper move/dtor, generated files) were excluded by construction.

Generated 2026-06-13. Counts by dimension: duplication 50, untested 43, comments 38,
conventions 24, large-function 21, dead-code 18, large-cpp-file 9, large-ui-component 7, coupling 4.

---

## Executive summary

The codebase is largely healthy and internally consistent: it follows its own Go-flavored C++ conventions, keeps comments lean, and has a strong end-to-end test discipline through the `tests/e2e` bun-over-wire harness. The audit surfaced no correctness-breaking defects. What it found instead is a small set of recurring, mechanical convention drifts plus a structural debt load concentrated in a handful of files that have outgrown the project's own stated module/partition pattern. Nearly every finding is either trivially batchable or a low-risk extraction that keeps the build green.

The 5 biggest themes:

- **The banned ternary `?:` is pervasive (~24 sites/files, the single largest convention gap).** CONVENTIONS.md:46 bans `?:` outright, yet it recurs across the engine (`assets.cppm` 40+, `renderer.cppm` ~30, `host.cppm` 9, scene/animation/rendering-impl/control) and the editor (`viewIdWire`, gizmo mappers). All are genuine control-flow ternaries on engine types, not third-party operator overloads. This is pure mechanical cleanup, much of it collapsible into a few named helpers (`boolFlag`, `gizmoOpDto`, `nativeGizmoHandleName`).
- **Five files have become god-modules that violate the project's own "split into interface partition + .cpp impl units" guidance.** `assets.cppm` (6570), `renderer_detail.cppm` (5154), `renderer.cppm` (3868), `store.ts` (2069), `AssetsPanel.tsx` (2012) each bundle 5-9 unrelated concerns, and three of them also host functions over 1000 lines (`initDescriptorResources` ~1440, `beginFrameGraph` ~1090). Each has a clear, documented seam to cut along.
- **Heavy copy-paste in three hotspots: GPU one-off command scaffolds, PSO builders, and editor `errorText`/thumbnail/rename patterns.** The same allocate/begin/submit/waitIdle command-buffer dance is hand-rolled ~15 times; five graphics-pipeline builders repeat a ~90-line scaffold; `errorText` is reimplemented verbatim in 4 editor files that ignore the existing `lib/flash.ts` export. Some of this duplication is an active correctness risk (the 16-field `ViewTargets` reset duplicated between `destroyRenderer`/`destroyView` is a silent VMA leak waiting to happen).
- **The wire contract is hand-maintained in 3-4 parallel places and has already drifted.** `gen.ts` spells out the scene-component catalog three times (TS / OpenRPC / C++ serde), the Material field set 4+ times, and enum wire-strings in 3 tables. `protocol/index.ts` re-declares ~14 interfaces that the generator already emits, and two (`Material`, `Camera`) are already missing fields the generated types carry — so panels are typed against stale shapes.
- **Migration/change-journey and banner comments persist despite the NO-LEGACY rule, and a few are dead-code shims.** ~38 comment findings: `// --- ... ---` dividers, "legacy"/"replaces"/"phase N"/"as before" phrasing, and — worst — comments and code that point at a deleted DX11-era C++ editor (`editor_panels.cpp`, `editor_app.cppm`) that does not exist in the tree. Two are real compat shims to delete: the `asset_registry.json` migration and `METRICS_WINDOW_LEGACY_KEY`. One whole component (`MenuBar.tsx`) is dead.
- **Pure, edge-case-prone logic is broadly untested because there is no C++ unit harness and editor unit tests are thin.** 43 untested findings cluster on pure functions: barrier derivation, `floatToHalf`, gizmo/clip math, keybinding parse/match, frame-series bucketing, material graph round-trip, and a cluster of control commands (asset-folder ops, set-atmosphere, asset-usages) with no e2e coverage at all.

## Findings by dimension

### Conventions

| File:lines | Severity | Issue | Recommended fix |
|---|---|---|---|
| engine/source/saffron/assets/assets.cppm:320,805,1490,1508,2384,2463,5344,5454,5677,6071,6086,6123 (40+ sites) | high | Prohibited ternary `?:` in general control flow (path fallback, double-getenv, char fold, default material, decode dispatch). JSON-boundary cases at 634/654/660/1617/1619 are exempt. | Rewrite each as `if`/`else`; for 1508 assign the getenv once then branch. |
| engine/source/saffron/rendering/renderer.cppm:266,361-362,876,1118-1122,1221,1357,1591,2475,3413,3419-3424 (~28 sites) | high | Prohibited ternary throughout (timestampMask, gpuScopeRecorder/perfBudgetMs returns, EMA one-liners, parentIndex rebasing). | Convert to if/else; value-returners become if/else with two returns. |
| engine/source/saffron/rendering/renderer_thumbnail.cpp:266,303,555-556,604,643,734-735,772,807-809,827 | high | Prohibited ternary (storeOp, Image& selection, white/idx, submeshMaterials). | if/else into a pre-declared variable. |
| engine/source/saffron/rendering/renderer_lighting.cpp:120,127,132-135,140,153,178,315 | high | Flag-packing built almost entirely from ternaries in setSceneLighting/submitReflectionProbes. | Add `boolFlag(bool)->u32` helper; rewrite probeCount/sampleCount as if/else. |
| engine/source/saffron/rendering/renderer_pipelines.cpp:66,91,93,296 | high | Ternary in newMeshPipeline/newOverlayPipeline (skinned pName, binding/attr counts, depthTestEnable). | Plain if/else assignment. |
| engine/source/saffron/rendering/renderer_textures.cpp:94,131-132,147-153,201 | high | Ternary in mipCount/recordMipChain/uploadTexture (the `last ?` mip-transition cluster). | Compute fromLayout/fromStage/fromAccess in one if/else before mipBarrier. |
| engine/source/saffron/host/host.cppm:283,353,409-410,460,466,482,1057,1091 | high | 9 native ternaries (size/target/color selection, editorPid, animMode). | Hoist locals via if/else, then pass. |
| engine/source/saffron/animation/animation.cpp:105,314,341,536,591,593,685 | high | 7 ternaries (ping-pong delta, joint lookup, IK angle, clip load, CrossFade-vs-Inertialize). | if/else; for 685 assign into finalLocal[i]. |
| engine/source/saffron/scene/scene.cppm:615,630-632,700-701,877,947,950,954-955,1232 | high | 9 ternaries (hierarchy walk, relinkWarning, jointMatrices clamp, setParent). | if/else or small named helper. |
| engine/source/saffron/control/control_commands_asset.cpp:111,117,148,193-197,412,424-429,1035,1157,1573,2152-2154 (31 sites) | high | 31 ternaries; JSON-boundary reads included but the ban is on the operator. | Extract `selectorString(json)`/`optionalFolder(string)` helpers; convert the corner-loop/in-rig branches to if/else. |
| engine/source/saffron/control/control_commands_scene.cpp:81-85,239-240,248-250,1317-1324,1425-1434 | high | Nested ternaries (gizmoOp/space mapping both directions; 8-deep NativeGizmoHandle->name chain). | Extract `gizmoOpDto/FromDto`, `gizmoSpaceDto/FromDto`, `nativeGizmoHandleName` (switch). |
| engine/source/saffron/control/control_commands_animation.cpp:33,42,63,72,82,387 | medium | Ternary (selector decode, joint clamp). | if/else or local. |
| engine/source/saffron/control/control_commands_render.cpp:178,249,263,275-277,392,402,506 | medium | Ternary (profileLaneDto, durUs, tid, mode->string). | if/else; profileLaneDto -> switch like its sibling DTO mappers. |
| engine/source/saffron/geometry/geometry.cppm:552,583,689,810,824-825,869,930-937 | medium | 15+ ternaries incl. a hard-to-read nested chain at 930-937 (track path/interp). | Extract `toTrackPath`/`toTrackInterp` free fns; if/else elsewhere. |
| engine/source/saffron/sceneedit/scene_edit_context.cpp:52-60 | medium | Ternary in gizmoSpaceName/FromName; sibling gizmoOp* use switch/if. | if/else to match the in-file pattern. |
| engine/source/saffron/sceneedit/scene_edit_gizmo.cpp:25-28 | medium | Chained ternary GizmoOp/Space -> Native in syncNativeGizmo. | switch/if (matching gizmoOpName style). |
| engine/source/saffron/control/control_server.cpp:88 | low | viewIdWire ternary; its inverse viewIdFromWire already uses an if-chain. | if/else for the inconsistent half. |
| engine/source/saffron/animation/animation.cpp:341,372 | low | Bare `int` instead of the project `i32` alias (rest of file uses i32). | Use `i32` for consistency. |

### Comments

| File:lines | Severity | Issue | Recommended fix |
|---|---|---|---|
| editor/src/components/AssetTile.tsx:1-8,20-22,110-114,145-146 | high | Doc comments cite deleted DX11-era C++ (`editor_panels.cpp:226-325`, `editor_components.cpp:77`, `editor_app.cppm:138`) with "did before" phrasing; none of those files exist. | Rewrite to present-tense behavior; keep only the `application/x-se-asset` payload contract. |
| editor/src/components/AssetPicker.tsx:1-12,121 | high | Header frames component as a port of `drawAssetPicker (editor_components.cpp:21-84)` (deleted) with stale line cites. | Describe as an asset-Uuid combo + drop target; drop all `editor_components.cpp:NN` cites. |
| editor/src/control/client.ts:133,172,215,220,241,282,336,359,372,384,543,576,581,638,695,749 | high | 16 `// --- section ---` banner dividers (prohibited unconditionally). | Delete; if grouping matters, split into per-area modules (see large-UI finding). |
| engine/source/saffron/control/control_dto.cppm:1352 | medium | `ListClipsParams::entity` "accepted for wire-compat; ignored" — a compat shim. | Delete the field; update list-clips call site + generator fixture + editor client.ts:244. |
| engine/source/saffron/assets/assets.cppm:921,1589,6010 | medium | `// --- ... ---` / `// === ... ===` dividers. | Delete; fold prose into the first decl's `///`. |
| engine/source/saffron/rendering/renderer_detail.cppm:402,3726,4004 | medium | `// ---- ... ----` dividers (RT/DDGI/TLAS sections). | Delete; fold into the first decl as extractions land. |
| engine/source/saffron/json/json.cppm:33-36,41 | medium | "older files written with bare numbers still load" — rationalizes a migration that cannot exist on orphan main. | State present behavior ("accepts a JSON number or decimal string"); if it's a JS-precision concern, say that. |
| editor/src/panels/AssetsPanel.tsx:1-7,70-71 | medium | Header is a change-journey port-of reference to deleted C++ with rotting line numbers. | Describe the panel as-is; drop the C++ provenance. |
| editor/src/state/store.ts:74,149,605,1389-1390,1475-1476,1744 | medium | "migrated once", "replacing the old tool slices", "legacy", "Phase 07 retires…" (stale), "decoupled from". | Drop the historical clause in each. |
| engine/source/saffron/geometry/geometry.cppm:1184 | medium | `hash ^ u64{0}` "NUL separator" comment — XOR-with-0 is identity; separation comes only from the next `* fnvPrime`. | Drop the dead XOR (fix comment) or fold a real separator byte. |
| engine/source/saffron/geometry/geometry.cppm:534-538 | medium | Stacked, contradictory blurbs above readGltfTextureBytes ("textures intentionally skipped" vs reading them). | Move 534-535 down to extractGltfMaterial; reconcile the "skipped" claim. |
| engine/source/saffron/scene/scene.cppm:350-351 | low | "supersedes the older AssetEntry.linear bool" — and `linear` still exists at 368 (incomplete cutover). | State what Colorspace is; reconcile the lingering `linear`. |
| engine/source/saffron/scene/scene.cppm:1303-1304 | low | "Replaces the old ECS smoke test." | Delete the sentence. |
| engine/source/saffron/scene/scene.cppm:374 | low | "texture colorspace provenance (phase 10 fills it)" plan-phase artifact. | State the field meaning; drop the phase ref. |
| engine/source/saffron/host/host.cppm:968,970 | low | "then legacy root project.json" change-journey wording. | Describe current auto-load behavior without "legacy". |
| engine/source/saffron/control/control_dto.cppm:1284 | low | "(Phase 12 reconcile poll)" — match the present-tense sibling at 1275. | Rewrite to behavior. |
| engine/source/saffron/control/control_dto.cppm:246 | low | "append-last (positional aggregate init)" per-field restatement of a project-wide rule. | Trim to "debug render-output mode". |
| engine/source/saffron/rendering/renderer.cppm:2942-2943 | low | Comment poses and rejects a design alternative (deliberation noise). | State only what restorePass does. |
| engine/source/saffron/rendering/renderer.cppm:2819 | low | "clear it here as before" change-journey phrasing. | Drop "as before". |
| engine/source/saffron/rendering/renderer_detail.cppm:3054,60 | low | "are now PER-VIEW"; "the earlier one-off paths". | State present fact / plain dependency. |
| engine/source/saffron/rendering/renderer_types.cppm:589-593,535-537 | low | DrawBatch "batches no longer split by texture"; Material "For v1…" (also Restir 1643/2076). | Present-tense restatement; drop the v1 framing. |
| engine/source/saffron/animation/animation.cpp:531,552,557 | low | Numbered `// 1./2./3.` step markers (borderline, content-bearing). | Drop numerals, keep prose if touched. |
| editor/src/panels/HierarchyTree.tsx:188-191,196-197 | low | "matching the old per-row triggers". | State the empty-area suppress behavior in present tense. |
| editor/src/panels/AssetEditorWorkspace.tsx:49-53,249 | low | "the old fixed 0.2 floor"; "(NO-COMPAT)" banner token. | State current rationale; drop the token. |
| editor/src/app/CreateMenu.tsx:27-32 | medium | Garbled/truncated doc fragment referencing the dead "phase-8 menu bar" + `editor_app.cppm:101-107` cite. | Rewrite to one or two present-tense lines; drop line cite + dangling fragment. |
| editor/src/app/MenuBar.tsx:4-8 | low | Header "mirrors the C++ Create + File menus" for a dead component. | Subsumed by deleting the file. |
| editor/src/components/timeline/{TimelineSurface.tsx:5,TimelineTransport.tsx:3-4,shared.ts:1-2} | low | "move out of TimelinePanel" / "factored from TimelinePanel". | Describe present behavior, drop prior-location notes. |
| editor/src/lib/timelineCanvas.ts:8-10,42,288 (+TimelinePanel.tsx:7) | low | Over-narrated "unused by v1" Phase-13 scaffolding repeated 4x. | One concise "/// Authoring lane (not yet wired)." |
| editor/src/lib/useSubsurfaceBounds.ts:1-6 | low | "(the stale-frame bug that fix removes)" past-contrast. | State the present invariant. |
| editor/src/app/useGizmoShortcuts.ts:5-11 | low | "(spike-0b, recorded in the phase-3 migration plan)" dangling provenance. | Keep the substantive explanation; drop the parenthetical. |
| engine/source/saffron/assets/assets.cppm:453-454 | low | "stay byte-identical to before this field set existed" change-journey. | State present intent (omit default container/colorspace). |
| engine/source/saffron/control/control_commands_scene.cpp:795-797 | low | set-environment help omits `exposure?` though body handles it (DTO field exists at control_dto.cppm:1245). | Add `exposure?` to the help string. |

### Dead code

| File:lines | Severity | Issue | Recommended fix |
|---|---|---|---|
| editor/src/app/MenuBar.tsx:38-268 | high | Entire `MenuBar` component is dead — no importer anywhere (live menus are Topbar->ProjectMenu, HierarchyPanel->CreateMenu, which uses only `CREATE_PRESETS`). | Delete the file; `CREATE_PRESETS` already lives in CreateMenu.tsx. Resolves the `errorText`/`rememberProject` dupes too. |
| engine/source/saffron/rendering/renderer_types.cppm:1899-1902 + renderer_textures.cpp:42-90 | medium | Exported `uploadSvgIcon` (whole nanosvg path) has zero callers; editor uses Lucide DOM icons. | Delete decl+def; drop the nanosvg include if now unused. |
| engine/source/saffron/rendering/renderer_types.cppm:1998-1999 + renderer_lighting.cpp:257-260 | medium | `requestSkyBake` is a dead wrapper over `requestEnvBake`; the one procedural caller (assets.cppm:5880) calls requestEnvBake directly. | Delete (NO-COMPAT duplicate path). |
| engine/source/saffron/control/command.cppm:79,88,91 + control_server.cpp:64,155 + control_commands_render.cpp:426 | medium | Exported `asString`, `entityRef(Scene&,Entity)->json`, `renderStatsJson` have zero callers; entityRefDto/renderStatsDto are the live path. | Delete all three; the DTO functions remain the single serialization path. |
| engine/source/saffron/assets/assets.cppm:335-342,4769 | medium | `projectInfoJson` (saveProject serializes inline instead) and `spawnMesh` (live API is spawnModel/spawnSkinnedModel) have no callers. | Delete both. |
| engine/source/saffron/geometry/geometry.cppm:232-233,1276-1296 | medium | `bakeDxToGlNormal`/`bakeGlossToRoughness` exported, zero callers; comments claim "baked at import" (untrue). | Delete or wire into the import path. |
| engine/source/saffron/geometry/geometry.cppm:981-989,1136-1157,1379-1382 | medium | Mesh-returning `importGltf`/`importObj`/`importModelFile` + file-path `saveMesh` used only by the self-test; production uses translateModel + writeContainer. | Delete and have the self-test use the production path, or justify keeping as API. |
| engine/source/saffron/control/control_commands_asset.cpp:87-107 | medium | `assetSlotName` defined in anon namespace, referenced nowhere (truly dead). | Delete. |
| editor/src/state/store.ts:74,1389-1390,1477-1492 | medium | `METRICS_WINDOW_LEGACY_KEY` one-time-migration read (NO-COMPAT violation); the "sidebar-width helpers below" comment is dangling/stale. | Delete the legacy-key read + constant; loadMetricsRangeSec reads only METRICS_RANGE_STORAGE_KEY; drop the stale clause. |
| editor/src/components/ui/card.tsx:1-76 | low | Vendored Card primitive, zero importers. | Delete; re-add via shadcn if needed. |
| editor/src/components/ui/tabs.tsx:1-82 | low | Vendored Radix Tabs primitive, zero importers (dock tabs are a separate custom system). | Delete; re-add via shadcn if needed. |
| editor/src/state/dockLayout.ts:98-100,102-104 | low | Exported `leafActiveTab` and `isLeafEmpty` have zero callers. | Delete both. |
| engine/source/saffron/rendering/renderer_types.cppm:2132,2161,2055 (+renderer.cppm defs) | low | `defaultTexture`, `profileCaptureReady`, `screenEffectsEnabled` exported, zero readers (live accessors are the field / profileCaptureState / the individual gates). | Delete decl+def of each. |
| engine/source/saffron/rendering/renderer_types.cppm:1810,1837,1838 (+renderer.cppm defs) | low | `viewportColorResource`, `viewportImageView`, `viewportGeneration` have zero callers (viewportWidth/Height are live). | Delete the three. |
| engine/source/saffron/control/command.cppm:88 (entityRef) | low | Duplicate of the above entityRef finding; every call site uses entityRefDto. | Delete. |
| engine/source/saffron/assets/assets.cppm:335-342 (projectInfoJson) | low | Exported, zero callers (low-confidence: could be intended API). | Delete unless a project-info command should route through it. |
| editor/src/components/dock/useTabStripDrag.ts:50,100-102 | low | `UseTabStripDragOptions.isDraggable` never supplied (canDrag always hits `?? true`). | Drop the option or mark it an intentional seam. |
| editor/src/app/WindowTitlebar.tsx:146-154 | low | `tabIcon` branches for assetType 'mesh'/'animation' unreachable (imageViewer tabs are only textures/other; model/mesh/animation route to assetEditor). | Drop the two branches (leave texture->ImageIcon, else File). |
| editor/src/lib/timelineCanvas.ts:23-24,41-46,234-237,288-314 | low | diamonds draw path / TimelineKey / keys[] wired but never exercised (mode always "bars", keys always []). | Keep as documented Phase-13 scaffolding or remove for a strict clean-slate posture; do not let it grow. |
| tools/gen-control-dto/gen.ts:1382-1385 | low | Generated `optionalField` is a pure pass-through to `fieldValue`. | Call fieldValue directly, or make optionalField encode the intent. |
| tools/gen-control-dto/gen.ts:1214-1220 | low | `cppJsonValue` has a `WireUuid` case identical to `default` and a `Json` case identical to the scalar group. | Drop WireUuid case; fold Json into scalar group. |
| editor/src/control/client.ts:702-704,531-541,237-239,470-476,527-529,278-280 | low | 9 typed wrappers with no UI caller (loadProject duplicates openProject; assetReferences, cleanAssets, deleteUnused, getPlayState, materialAssign/Import, modelInfo, stopPreview). Commands stay live via the generic passthrough. | Remove uncalled wrappers (at least loadProject), or make the exhaustive-API intent explicit. |

### Coupling / separation of concerns

| File:lines | Severity | Issue | Recommended fix |
|---|---|---|---|
| engine/source/saffron/sceneedit/scene_edit_gizmo.cpp:212-348,350-470 | medium | hitNativeGizmo/snapshot/applyDrag/rebase read `editor.scene` directly while stepEditSmoothing uses `activeScene(editor)` — contradicts the module's own sole-accessor invariant; Begin phase checks `activeScene` (1393) then snapshots `editor.scene` (1402), a real cross-scene inconsistency during preview. | Route the gizmo through activeScene, or document the Edit-only exemption at each site. |
| editor/src/panels/HierarchyTree.tsx:25,361,476 | medium (low in TS slice) | HierarchyTree imports `orderedComponentNames` + COMPONENT_ORDER/HIDDEN_COMPONENTS from InspectorPanel (panel-to-panel reach for shared policy). | Move to a neutral `lib/componentOrder.ts` both panels import. |
| editor/src/components/AssetTile.tsx:23-88 (imported by HierarchyTree:22, ViewportPanel:13) | low | DnD wire protocol (ASSET_DND_MIME/readAssetPayload/assetIdsFromPayload) lives in a view component; drop targets import the whole tile module for a MIME string. | Extract `lib/assetDnd.ts`; re-export from AssetTile if convenient. |
| editor/src/components/timeline/TimelineSurface.tsx:92-93,136,188,193,200,204-229 | medium | Imperative effect calls `useEditorStore.getState()` and hand-diffs store-field identities (animationState/animationClips/componentsBySelected). | Keep imperative (justified) but subscribe with a selector + equalityFn so the manual triple-snapshot diff collapses; pass the snapshot into the extracted deriveModel. |
| editor/src/* (21 files using getState() outside selectors) | low | getState() is becoming the default access mode in render-adjacent code (AssetTile, renderLog), hiding which components depend on which slices. | No change to documented handler cases; when the store is split, audit each getState() reads only its own slice. |

### Duplication

| File:lines | Severity | Issue | Recommended fix |
|---|---|---|---|
| editor/src/protocol/index.ts:182-299 | high | ~14 interfaces hand-redeclared instead of re-exported from generated se-types.ts; `Material` (215-223) missing `metallicRoughnessTexture`, `Camera` (182-187) missing showModel/showFrustum/frustumMaxDistance — already drifted, and these shadow the generated shapes (InspectorPanel uses metallicRoughnessTexture against the stale type). | Delete every hand interface duplicating a generated one; `export type {…} from "./se-types"`; derive Environment/Selection from the DTO. |
| tools/gen-control-dto/gen.ts:1719-1891,1983-2300,2460-2909 | high | Scene-component catalog hand-written 3x (TS / OpenRPC / C++ serde); Material set 4+x; already drifted — TS Material has 8 fields vs 16 in OpenRPC+C++, and ModelInstance is in componentNames+ComponentBody but absent from TS Components. | Define one in-code `SCENE_COMPONENTS` table; derive all three emitters from it (at minimum a single Material descriptor). |
| engine/source/saffron/rendering/renderer_detail.cppm:1592-2097 | high | 6 graphics-pipeline builders (depthPrepass/shadow/pointShadow/gbuffer/motion/sky) repeat a ~90-line PSO scaffold differing only in sample count/depthBias/formats. | Introduce `GraphicsPipelineDesc` + one `buildGraphicsPipeline(renderer, desc)`; removes ~400 lines and a wrong-sample-count class of bugs. |
| editor/src/lib/flash.ts:57-65 + ProjectMenu.tsx:205-213 + ProjectStartupModal.tsx:267-275 + MenuBar.tsx:272-280 + RenderStatsPanel.tsx:468-476 | high | `errorText` reimplemented byte-for-byte in 4 files despite the canonical export. | Delete the four copies; `import { errorText } from "../lib/flash"`. |
| engine/source/saffron/rendering/renderer.cppm:628-646,3703-3719 (+588-594,3727-3731) | medium | The 16-field ViewTargets image-reset (and the RestirView reset) duplicated verbatim between destroyRenderer/destroyView — a silent VMA-leak risk when a new per-view image is added to one site only. | Extract `resetViewImages(ViewTargets&)` / `resetRestirView(RestirView&)`, call from both. |
| engine/source/saffron/rendering/{renderer_drawlist.cpp,renderer_textures.cpp,renderer_thumbnail.cpp,renderer_detail.cppm} (~15 sites) | medium | One-off command-buffer scaffold (alloc + begin OneTimeSubmit + record + end + submitAndWait/free) hand-rolled everywhere; only the recorded body and label vary. | Add `withOneOffCommands(renderer, label, record) -> Result<void>` in :Detail; collapse call sites to one call. |
| engine/source/saffron/rendering/renderer_detail.cppm:906-917,2197-2203,2439-2445,2562-2568,2855-2861 (12x) | medium | Init-path `submit2 + device.waitIdle() + freeCommandBuffers(frame[0].commandPool)` tail repeated — a second, blunter idiom alongside fence-based submitAndWait. | Route through withOneOffCommands/submitAndWait (the fence is strictly better), or extract submitAndWaitIdle. |
| engine/source/saffron/rendering/renderer_detail.cppm:64-81 + ~6 caller lambdas | medium | `transitionImage` hard-codes a single-mip/single-layer subresource, so callers re-roll the full barrier as mipBarrier/cubeBarrier/inline blocks. | Add a subresource-range overload; delete the local barrier lambdas. |
| engine/source/saffron/rendering/{renderer_textures.cpp:183-198,355-370 ; renderer_drawlist.cpp:97-118} | medium | Host-mapped TRANSFER_SRC staging-buffer recipe copied across mesh/texture uploads. | Extract `createStagingBuffer(renderer, bytes)` (or a StagingBuffer RAII wrapper). |
| engine/source/saffron/rendering/renderer_thumbnail.cpp:185-209,246-307,584-647,752-831 | high (medium in cross-cut) | renderMesh/Material/Model thumbnails share a near-verbatim offscreen-render scaffold (MSAA alloc, transition triplet, attachment setup, final transition, identical 13-line GpuTexture handoff). | Extract beginThumbnailTarget/finishThumbnailTarget (or withThumbnailRender); callers differ only by the draw closure. |
| engine/source/saffron/rendering/renderer_thumbnail.cpp:68-167,343-443 | medium | newThumbnailPipeline / newPreviewPipeline ~80% identical PSO builders. | One helper parameterised on shader path, push range/stages, optional bindless set. |
| engine/source/saffron/rendering/renderer_textures.cpp:175-299,341-461 | medium | uploadTexture / uploadTextureFloat duplicate the full staging-upload pipeline (288-298 vs 450-460 byte-identical). | Extract `uploadTextureRaw(...)`; both entry points marshal then delegate. |
| engine/source/saffron/assets/assets.cppm:2061-2096,2103-2148,2154-2199 | medium | slangc compile boilerplate duplicated across 3 codegen functions. | Extract `compileSlangToSpv(slangPath, spvPath, extraArgs)`. |
| engine/source/saffron/assets/assets.cppm:1625-1636,1735-1746,2231-2242 | medium | Identical uuid-from-json lambda copied 3x. | Extract `uuidFromJson(const json&) -> Uuid` at the serde boundary. |
| engine/source/saffron/assets/assets.cppm:942-954,1050-1093,2936-2956,3650-3691 | low | FNV-1a basis/prime + mix open-coded in 4 places. | Provide a `Fnv1a` helper (init + mix + mixBytes). |
| engine/source/saffron/scene/scene.cppm:186-229 | medium | MaterialSlot duplicates every MaterialComponent field verbatim. | Extract shared MaterialParams struct; coordinate with gen.ts (serde generated). |
| engine/source/saffron/scene/scene.cppm:651-663,1364-1376 | low | Self-test re-implements a find-entity-by-name walk mirroring findEntityByUuid. | Promote to a free fn if useful, else leave test-local. |
| engine/source/saffron/control/control_commands_asset.cpp:114-143,145-175 | medium | resolveAsset / resolveAssetIndex duplicate the entire selector-parsing block. | Extract `decodeAssetSelector(AssetSelector)`; implement resolveAsset on resolveAssetIndex. |
| engine/source/saffron/control/control_commands_asset.cpp:285-316,318-354 | medium | collectAssetUsages / clearAssetUsages near-identical traversals (a new slot must be added to both). | One helper with a `bool clear`/callback. |
| engine/source/saffron/control/control_commands_asset.cpp:1620-1711 | medium | assign-asset: 7 near-identical ensure-component/get-field arms (6 are MaterialComponent fields). | switch mapping AssetSlotDto -> pointer-to-member; ~60 lines -> ~10. |
| engine/source/saffron/control/control_commands_asset.cpp:1633-1639,1737-1740,1383-1389 | low | Clear-sentinel test spelled twice (material-assign omits the integer-0 case — latent inconsistency); rename-asset re-implements the selector decode. | Extract `isClearSelector(json)`; route through decodeAssetSelector. |
| engine/source/saffron/control/control_commands_animation.cpp:30-56,60-86 | medium | resolveClip / resolveContainer duplicate the selector decode (already drifting: one filters AssetType::Animation, one doesn't). | Share `decodeAssetSelector`. |
| engine/source/saffron/control/control_commands_scene.cpp (375-383,475-483,530-536,296-304,319-323) | low | "findByName -> registered -> has" pattern repeated with hand-written, drifting error strings. | Add `requireComponent(ctx, entity, name)` / `requireRegistered(ctx, name)` in command.cppm. |
| engine/source/saffron/rendering/renderer_types.cppm:96-149,153-222,…,1492-1555 | medium | 7 move-only RAII wrappers repeat copy-delete/move/dtor/reset; Image/GpuTexture/Image3D reset() bodies are identical image+view+alloc teardown. | Factor `freeImage(device,allocator,image,view,alloc)` (+ small free helpers) shared by the three reset()s. |
| engine/source/saffron/rendering/renderer_detail.cppm:3511-3536 (+3568,3783,3841,4063,4120,4450,4803) | medium | makeComputeLayout/makeLayout/rsLayout + 4 allocSet variants reimplement "ordered single-count layout" / "allocate-one-set". | Hoist `makeOrderedSetLayout(...)` + `allocateSet(...)` free helpers. |
| engine/source/saffron/rendering/renderer_detail.cppm:1456-1531 (+renderer.cppm:3075) | low | make*StorageBuffer helpers share an identical Buffer-construction body. | Extract private `makeBuffer(renderer, bytes, usage, hostMapped, label)`; keep named wrappers. |
| {scene.cppm:900,scene_edit_gizmo.cpp:345,443,assets.cppm:4828,scene_edit_context.cpp:74} | low | quat->engine-Euler (extractEulerAngleZYX after mat4_cast) copied 4+x with repeated stability comment. | Add `quatToEulerZYX(quat)->vec3` to Saffron.Scene. |
| engine/source/saffron/script/script.cppm:167-176 + script_runtime.cpp:193-202 | medium | `tracebackHandler` duplicated verbatim across both TUs. | Hoist one internal helper, reuse from both. |
| engine/source/saffron/script/script_runtime.cpp:257-279,618-634 (+script.cppm:180-204) | medium | load-chunk-then-require-table boilerplate repeated 3x. | Extract `loadChunkReturningTable(L, path)`. |
| engine/source/saffron/sceneedit/scene_edit_gizmo.cpp:487-521,560-631 | low | material/transform smooth-entry find-or-emplace + cancel clones; the converge `field` lambda duplicated verbatim. | Template `smoothEntryFor<T>`/`cancelSmoothing<T>`; shared convergeField. |
| editor/src/state/store.ts:954-1002,1067-1126 / 1477-1550 / 732-790 | low | persist-to-localStorage try/catch repeated 8x; load-Number-validate-or-default repeated; openX Tab focus-or-append repeated 4x. | `persistPref`/`loadNumberPref`/`loadBoolPref`; `focusOrAppendTab(s,id,makeTab)`. |
| editor/src/components/AssetTile.tsx:164-196 + AssetPicker.tsx:33-58 (+AssetViewer near-sibling) | high | Lazy thumbnail fetch state machine (cache-seed + getThumbnailUrl + loading/ready/none + cancelled guard) duplicated. | Extract `useThumbnailUrl(id,size)` hook; keep per-component fallback JSX. |
| editor/src/components/{AssetTile.tsx:296-323,HierarchyTree.tsx:516-567,AssetFolderTree.tsx:445-510} (+AssetsPanel NewFolderTile/FolderNameInput) | medium | 3+ near-identical inline-rename inputs (autofocus-select, Enter/blur-commit settled guard, Escape-cancel) with subtly different guards. | Extract one `InlineRenameInput` / `InlineNameInput` component. |
| editor/src/panels/EnvironmentPanel.tsx:94-137 (+InspectorPanel:153-163,291-314; MaterialEditorPanel:104-114) | medium | Per-field coalescer factory (Map + lazy makeCoalescer + fold-if-not-dragging) duplicated as env vs atmosphere and across panels. | `useFieldCoalescers<K,P>(send)` hook. |
| editor/src/components/{NumberDrag.tsx:55-84,VectorEditor.tsx:55-86} | medium | Pointer drag-scrub gesture copy-pasted; clamp duplicated in NumberDrag/SliderField. | `useDragScrub({step,onBegin,onUpdate,onEnd})` hook + shared `clamp` in lib/utils. |
| editor/src/panels/{RenderPanel.tsx:164-303,RenderStatsPanel.tsx:174-195,Topbar.tsx:92-104} | medium | Optimistic-write + echo-fold + revert-on-reject repeated per setter; `optimistic(patch)` defined twice verbatim. | Lift `optimistic` to a shared util; add generic `optimisticToggle(field,set,next,{revert})`. |
| editor/src/panels/{HierarchyTree.tsx,SkeletonTree.tsx,AssetFolderTree.tsx} (indent consts + tree chrome + group-by-parent) | medium | MAX_INDENT_DEPTH/INDENT_PX, twisty + selected-row classes, and children-by-parent grouping duplicated 3x (SkeletonTree even comments the copy). | Share indent consts + `treeRowIndent(depth)` + `groupByParent`/`buildForest`. |
| editor/src/app/{MenuBar.tsx:282-289,ProjectMenu.tsx:215-222,ProjectStartupModal.tsx:79-84} | medium | `rememberProject` defined identically 2x + inlined once. | Move into control/client.ts (or lib/recentProjects.ts) and reuse. |
| editor/src/app/MenuBar.tsx:59-69 vs lib/flash.ts:16-40 | low | Local `flash()` reimplements the `useFlash()` hook. | Use `useFlash()`. |
| editor/src/components/AssetViewer.tsx:20-27 vs state/store.ts:2007-2013 | low | base64-to-blob decode loop duplicated. | One `base64ToBlob(b64, mime)` in lib. |
| editor/src/panels/EnvironmentPanel.tsx:39-40 vs components/fieldRenderer.tsx:105-106 | low | RAD_TO_DEG/DEG_TO_RAD duplicated. | Export once from lib/utils.ts. |
| editor/src/panels/{InspectorPanel.tsx:452-497,MaterialEditorPanel.tsx:183-202} (+EnvironmentPanel Row) | low | Labelled field-row markup (humanizeFieldName Label + renderField) hand-written 4+x. | Extract a `FieldRow` component next to renderField. |
| editor/src/components/ScriptSlots.tsx:253-302 vs fieldRenderer.tsx:151-255 | medium | renderFieldWidget re-implements renderField's widget dispatch (identical Input className at both:272/247). | Factor a shared primitive dispatcher; both adapt their value model. |
| editor/src/panels/RenderStatsPanel.tsx:466-476 vs lib/flash.ts:57-65 | medium | errorText byte-for-byte duplicate (also a coupling issue — see below). | Import from lib/flash. |
| editor/src/panels/ViewportPanel.tsx:555-571 vs HierarchyTree.tsx:214-230 | medium | Model-asset drop handler duplicated verbatim (also recurs in AssetsPanel). | Extract `instantiateDroppedModels(dataTransfer): boolean`. |
| editor/src/panels/ViewportPanel.tsx:89,97,401-404 | low | UV-to-NDC (u*2-1,v*2-1) inlined 3x. | Hoist `uvToNdc(uv)`. |
| editor/src/app/ProjectMenu.tsx:215-222 (rememberProject) | low | Byte-identical to MenuBar (resolved by the MenuBar deletion). | Consolidate after MenuBar is gone. |
| editor/src/components/CaptureControls.tsx:275-362 | low | Wide and narrow trace-action menus duplicate the same 3 actions/handlers. | Define the action set once; render both shells from it. |
| editor/src/panels/RenderStatsPanel.tsx:44-51,54-68 | low | Stat and Metric differ only by optional status coloring. | Collapse to one component with optional `status`. |
| editor/src/components/dock/useTabStripDrag.ts:158-174 vs dockDrag.ts:77 | low | insertionIndexForPointer duplicates insertionIndexForCenters ("first center left-of, else end"). | Factor a shared scan; hook layers the moving/pinned filter. |
| editor/src/lib/keybindings.ts:228-238 vs ViewportPanel.tsx:313-329 | low | Fly-camera hold matching reimplemented inline instead of matchesBinding. | Route through matchesBinding or expose matchesHoldCode. |
| tools/gen-control-dto/gen.ts:63-107,1638-1671,1929-1953 | medium | Enum wire-strings in 3 parallel tables (enumWireNames / tsType unions / jsonSchemaFor). | Derive the tsType union from enumWireNames; drop per-enum tsType cases. |
| tools/gen-control-dto/gen.ts:1274-1278,2957-2961 | low | Field types validated twice (main blanket pass + emitCpp subset). | Single `validateAll(structs,enums)` called once in main. |
| tools/check-control-schema/check.ts:154-178 vs tests/e2e/harness.ts:97-133 | low | Socket call() request/response loop duplicated across two tool trees. | Optional shared `callControl(socket, cmd, params, opts)` exposing both result shapes. |

### Large files

| File:lines | Severity | Issue | Recommended fix |
|---|---|---|---|
| engine/source/saffron/assets/assets.cppm:1-6570 | high (rated medium in slice) | 6570-line single-file module; assets/AGENTS.md already maps 5 disjoint concerns. | Keep declarations in the interface unit; move bodies into `module Saffron.Assets;` impl units: project, material, import/container, scene (renderScene/pickEntity/spawn), thumbnail (cleanest first cut — only threaded concern). |
| engine/source/saffron/rendering/renderer_detail.cppm:1-5154 | high | 5154-line :Detail partition with 8 helper families + the ~1440-line initDescriptorResources. | Peel into renderer_accel.cpp / renderer_env.cpp / (existing) renderer_pipelines.cpp; decompose initDescriptorResources into per-subsystem init fns. |
| engine/source/saffron/rendering/renderer.cppm:1-3868 | medium | Interface unit inlines newRenderer (~730), beginFrameGraph (~1090), and the perf/alarm subsystem. | Move bodies into renderer_init.cpp / renderer_framegraph.cpp / renderer_perf.cpp; keep declarations + trivial accessors. |
| engine/source/saffron/geometry/geometry.cppm:1-2112 | medium | Mixes types, glTF/OBJ import, stb decode, .smesh/.sanim/.smodel serde, self-tests. | `:Types` partition + geometry_import.cpp / geometry_image.cpp / geometry_container.cpp / geometry_selftest.cpp; isolates each vendored header. |
| engine/source/saffron/control/control_commands_asset.cpp:1-2298 | medium | 2298 lines, ~5x its siblings; bundles project/catalog+folders/import+preview/materials/capture. | Split into control_commands_project.cpp / _material.cpp / _asset_preview.cpp, each with its own register fn. |
| engine/source/saffron/rendering/renderer_types.cppm:1-2211 | medium | 2211-line catch-all :Types partition aggregating ~30 struct families + 132 free-fn decls; widest fan-in / merge-conflict surface. | Split into :Resources / :Profiler / :Types proper, or sub-group subsystem structs into partitions (lower priority — keeping device-state in one place has value). |
| engine/source/saffron/control/control_dto.cppm:1-1872 | low | 1872 lines but structurally flat; intentional single DTO source-of-truth for gen.ts. | Leave as-is; only split (by concern, re-exported by :Dto) if it keeps growing. |
| engine/source/saffron/scene/scene.cppm:1-1643 | medium | Mixes component structs, hierarchy math, ComponentRegistry, serde, and ~340 lines of self-tests in the shipping interface. | Interface partition + impl units; move the two self-tests out of the interface. |

### Large UI components

| File:lines | Severity | Issue | Recommended fix |
|---|---|---|---|
| editor/src/state/store.ts:1-2069 | high (medium in slice) | One flat EditorState god-object (~261 members) across ~9 domains. | Zustand slices (scene/assets/dock/metrics/capture/settings/history); public hook + selectors stay byte-identical. Metrics+capture is the cleanest first cut. |
| editor/src/panels/AssetsPanel.tsx:1-2013 | high | 2013-line file bundling panel + 6 tiles + context menu + breadcrumbs + overlay + DnD/marquee + 13 helpers. | Split into panels/assets/* (assetPath.ts, marquee.ts, AssetGrid.tsx, GridContextMenu, FolderTile, Breadcrumbs); pure helpers move first. |
| editor/src/control/client.ts:132-770 | medium | 773-line client object, 131 thin wrappers across ~14 domains, carved by 16 banners. | Split into control/{scene,assets,materials,perf,render,lifecycle}.ts sharing the `call` helper; compose into `client` so `Client = typeof client` is unchanged. |
| editor/src/components/ScriptSlots.tsx:45-478 | medium | 478-line component bundling schema fetch, slot CRUD, override coalescing, undo bracketing, widget dispatch, JSX. | Extract `resolveScriptRel` (pure), `useScriptOverrides` hook, `ScriptSlotFields` subcomponent. |
| editor/src/panels/InspectorPanel.tsx:78-580 | medium | Component body carries the full write/undo/coalescer machinery + render tree. | Extract `applyWrite`/no-op guard into panels/inspectorWrites.ts (client as param); makes routing assertable. |
| editor/src/app/App.tsx:55-323 | medium | App() runs 8 effects across distinct lifecycle concerns + the asset-mount state machine + 4 leaf components. | Extract `useWindowReveal`/`useEngineLifecycle`/`useUiFrameMeter`/`useProjectSync`/`useViewportRouting`; the EMA + mount machine become testable. |
| editor/src/panels/AssetEditorWorkspace.tsx:93-502 | low | 410-line component mixing orbit-easing, preview lifecycle, dock gating, overlay toggles. | Extract `useOrbitCamera(hostRef, framedDistance)` hook. |

### Large functions

| File:lines | Severity | Issue | Recommended fix |
|---|---|---|---|
| engine/source/saffron/rendering/renderer_detail.cppm:2753-4192 | high | initDescriptorResources ~1440 lines doing every one-time GPU setup. | Extract initShadowTargets / initLightAndClusterResources / initSsaoResources / initTaaResources / initDdgiResources / initRtAndRestirResources (each Result<void>). |
| engine/source/saffron/rendering/renderer.cppm:1950-3038 | high | beginFrameGraph ~1090 lines building 29 inline passes. | Extract addSkinPass / addShadowPasses / addScreenEffectPasses / addDdgiPasses / addRestirPasses / addScenePass / addAaPasses returning their RgResources. |
| engine/source/saffron/rendering/renderer_detail.cppm:4553-5153 | medium | bakeEnvironment ~600 lines (IBL image creation + atmosphere LUTs + convolution). | Extract allocateIblImages / bakeAtmosphereLuts / convolveEnvCube (the last shared with captureReflectionProbe 4416-4524). |
| engine/source/saffron/assets/assets.cppm:5506-5919 | medium | renderScene ~413 lines doing 8 jobs. | Extract gatherLights / buildStaticDrawList / buildSkinnedDrawList / fitDirectionalShadow / resolveSkyBake; renderScene orchestrates. |
| engine/source/saffron/assets/assets.cppm:6342-6569 | medium | requestThumbnail ~227 lines with a ~75-line nested Model branch. | Extract buildTextureJob/buildMeshJob/buildModelJob + dispatchThumbnail. |
| engine/source/saffron/rendering/renderer_drawlist.cpp:445-910 | high | submitDrawList ~465 lines (bucketing/material interning/flatten/skinning/upload). | Extract buildBuckets/buildMaterialParams(pure)/flattenBatches/wireSkinDispatches/computeRenderStats(pure). |
| engine/source/saffron/host/host.cppm:818-1203 | medium | runHost ~385 lines (config, command reg, project load, 3 lambda bodies). | Extract resolveStartupProject / registerScriptSchemaCommand / runSelfTests; hostUpdate / hostRenderUi free fns. |
| engine/source/saffron/geometry/geometry.cppm:607-979 | medium | importGltfModel ~370 lines (parse/vertex assembly/skin/clip decode). | Extract buildImportedNodes / buildImportedSkin / decodeGltfClip. |
| engine/source/saffron/scene/scene.cppm:1305-1489 | medium | runSceneSerializationSelfTest ~185 lines, 6 scenarios. | Extract each scenario as a helper returning a failure count. |
| engine/source/saffron/scene/scene.cppm:1493-1641 | low | runSceneHierarchySelfTest ~148 lines, 6 assertions. | Per-concern helpers. |
| engine/source/saffron/animation/animation.cpp:583-732 | medium | tickAnimation ~145-line lambda mixing 5 concerns. | Extract gatherRig / applyTransition / writeOverrides. |
| engine/source/saffron/animation/animation.cpp:734-1164 | low | runAnimationSelfTest ~430 lines (~10 scenarios; also covered by e2e). | Optional: split into named static helpers. |
| engine/source/saffron/control/control_commands_scene.cpp:173-1521 | low | registerSceneCommands ~1350 lines, 30+ command lambdas (matches the per-file convention). | Split into registerEntity/Component/Selection/Play/Environment/Viewport sub-fns. |
| editor/src/state/store.ts:1579-1986 | medium | startReconcile ~400 lines, ~20 mutable closure cursors, 4 lanes. | Extract makeFastTick/makeMetricsLane/makeWatchdog/makeAnimationLane + pure computeReconcileDeltas. |
| editor/src/panels/AssetsPanel.tsx:175-958 | high | AssetsPanel component ~780 lines, ~30 hooks, deeply nested JSX. | Extract useFolderHistory/useFolderMutations; lift pendingDelete IIFE; DeleteConfirmDialog component. |
| editor/src/components/timeline/TimelineSurface.tsx:73-251 | medium | Mount useEffect ~180-line closure bundling 6 concerns. | Extract pure advancePlayhead + deriveModel into lib/timelineModel.ts. |
| editor/src/panels/ViewportPanel.tsx:385-535 | low | Pointer useEffect ~150 lines (pick/drag-threshold/undo capture). | Extract pure `buildTransformUndoEntry(gesture, after)`. |
| tools/se/source/main.cpp:127-421 | medium | printResult ~295 lines, 27 per-command if-branches. | Per-command formatters + a dispatch table; pure helpers (bone-depth walk 184-195, base64 estimate 414-416) become testable. |
| tools/gen-control-dto/gen.ts:1677-1918 | medium | emitTs ~240 lines dominated by a hand-written literal. | Extract tsComponentInterfaces() + tsCommandMaps(). |
| tools/gen-control-dto/gen.ts:1983-2300 | medium | componentSchemas ~320 lines; required[] duplicates properties keys. | Per-component builders or generate from the shared component table. |

### Untested

| File:lines | Severity | Issue | Recommended fix |
|---|---|---|---|
| editor/src/lib/keybindings.ts:196-306 | high | parse/match/format/conflict logic (used in ~10 files) untested; subtle modifier-order/scope semantics. | keybindings.test.ts: normalizePressEvent ordering/null-on-modifier, matchesBinding hold-vs-press (code vs key), findConflict same-scope only, formatBinding cases. |
| editor/src/lib/frameSeries.ts (bucketSeries/appendFrameSamples/resetFrameSeries) | high | Downsampler + ring dedup/auto-reset back the live HUD; module-scope state, no test. | frameSeries.test.ts (resetFrameSeries in beforeEach): dedup overlapping windows, restart-reset on lower frameIndex, range<1 empty, exact maxBuckets with mean values. |
| editor/src/materials/graph.ts:154-200 | high | graphToFlow/flowToGraph wire<->ReactFlow round-trip; corruption is silent. | graph.test.ts: round-trip id/type/edges, editorPos fold/strip, unknown-type fallback, dangling-edge drop, posOf staggered fallback, freshNodeId monotonic. |
| editor/src/state/store.ts:1339-1357,1555-1567 | high (medium in slice) | buildTree / reanchorPastBones (consumed by HierarchyTree) untested; cycle guards + identity-stability contract. | tree.test.ts: root on '0'/missing/self/unknown, no loop on cycle, reanchor past bone chains, unchanged-parent returns identical object. |
| engine/source/saffron/control/control_commands_asset.cpp (asset-folder commands + validFolderPath/isFolderDescendant/replaceFolderPrefix, 219-265,1401-1511) | high | Cascade rename/move/delete path logic, zero e2e coverage. | tests/e2e/asset-folders.test.ts: reject `/bad`/`bad/`/`a//b`; rename `a`->`x` cascades to `x/b` + entries; delete clears entry.folder; sibling-prefix not touched. |
| engine/source/saffron/rendering/renderer_detail.cppm:1247-1308 | medium | convertToRgb / formatPixelBytes pure pixel logic (basis of every screenshot/thumbnail), no unit test. | Assert byte sizes, BGRA->RGB reorder, RGBA passthrough, half-float Clamp vs Tonemap math. |
| engine/source/saffron/rendering/render_graph.cppm:281-385 | medium | Barrier-derivation core (usageInfo/applyAccess/seedImageState) pure + correctness-critical, untested. | Assert layout transitions, hazard barrier on SampledRead-after-Write, buffer WAW single barrier, seed layout mapping. |
| engine/source/saffron/rendering/renderer_textures.cpp:304-339 | medium | floatToHalf pure f32->f16 (round-to-even, subnormal, overflow, nan), unreachable via e2e. | Assert known half-float values + overflow/inf/nan; also mipCount. |
| engine/source/saffron/rendering/renderer_drawlist.cpp:640-718,859-876 | medium | Submesh-major flatten + instance/RT/stats bookkeeping, no direct test. | After extracting flattenBatches/computeRenderStats, assert ordering, baseInstance/counts, dropped entity==0 placeholders, drawCalls/triangles. |
| engine/source/saffron/scene/scene.cppm:808-818,882-907 | medium | worldRotation scale-divide-out + setLocalFromMatrix Euler extraction (yaw +-90 instability) only tested transitively. | Direct checks for non-uniform scale and near-+-90 yaw. |
| engine/source/saffron/host/host.cppm:488-524 | medium | clipOverlayLine / clipToPixel pure clip-space math, no test. | runOverlayClipSelfTest or move to Saffron.SceneEdit; assert inside/outside/straddle + screen-center mapping. |
| engine/source/saffron/sceneedit/scene_edit_gizmo.cpp:65-114 | medium | ringBasis NaN-safety, pointInConvexQuad, pointSegmentDistance, viewportProject pure, no direct test. | runGizmoMathSelfTest: orthonormal ringBasis on world-up (no NaN), zero-length segment fallback, clip.w/ndc.z edges. |
| engine/source/saffron/control/control_commands_scene.cpp:856-920 | medium | set-atmosphere merge-over-current serde, no e2e. | atmosphere.test.ts: set a field, assert it persists + unrelated field unchanged + sceneVersion bumped. |
| engine/source/saffron/signal/signal.cppm:29-55 | medium | SubscriberList dispatch (stop-on-true, snapshot-iterate, erase-by-id) — engine-wide primitive, no test. | C++ test: call order + stop propagation, subscribe/unsubscribe mid-dispatch, monotonic ids. |
| engine/source/saffron/geometry/geometry.cppm:1172-1200,1278-1296 | medium | subIdFor stability + image bakes only in env-gated self-test. | e2e/native assert on subIdFor distinctness/floor + bake channel inversions. |
| engine/source/saffron/control/control_commands_asset.cpp (asset-usages/collectAssetUsages) | medium | Multi-component reference walk + slot labels + environment.skyTexture special case, no e2e. | Assert mesh/albedo/metallic-roughness/environment usages over the control plane. |
| editor/src/control/coalesce.ts (makeCoalescer) | medium | Single-in-flight/throttle/latest-wins write coalescer, no test. | coalesce.test.ts with fake timers: collapse-to-one, no concurrent send, throttle spacing, rejected-send swallowed. |
| editor/src/lib/alarmToasts.ts (routeAlarmToasts) | medium | Severity routing + resolve-surviving throttle, only engine side tested. | alarmToasts.test.ts mocking sonner: info=no toast, warn throttle survives resolve, critical persistent + dismiss prior. |
| editor/src/lib/perfThresholds.ts | medium | frameTime/vram/pass grading (HUD/engine agreement), no test. | Assert each band + boundary (median<=0 disables term, budget<=0 green). |
| editor/src/components/FrameTimeGraph.tsx:32-44,98-124,46-58 | medium/low | niceCeil, sticky/shrink-dwell Y-ceiling, maxFinite untested (and not extractable as written). | Extract to lib/axisScale.ts + pure nextSticky reducer; assert snap levels + hysteresis. |
| editor/src/components/CaptureTable.tsx:20-115 | medium | Pass-folding + pipeline-stat math pure but inline. | Extract foldGpuRows; assert fold/average, overdraw ratio, formatCount, budget=1000/fps. |
| editor/src/components/fieldRenderer.tsx:105-140 | medium | resolveHint/inferKind (incl. the radians 57x guard) untested. | fieldRenderer.test.ts: Transform.rotation convertRadians+deg, SpotLight.innerAngle deg-only, inferKind shapes. |
| editor/src/panels/AssetFolderTree.tsx:38-86 | medium | buildFolderTree/folderAncestorPaths/folderLabel pure, no test. | AssetFolderTree.test.ts: synthesized intermediates, ancestor list, label, flattenVisible DFS. |
| editor/src/components/AssetTile.tsx:32-90 | medium | DnD payload parsers (readAssetPayload/assetIdsFromPayload/readFolderPayload/isCatalogDrag) untested. | AssetTile.test.ts with stub DataTransfer: ids-over-id, malformed->null/[], string-only filter, MIME gate. |
| editor/src/panels/MaterialGraphEditor.tsx:67-78 | medium | graphsEqual (the undo gate) untested. | Assert order-insensitive equality + inequality on added edge/changed prop. |
| editor/src/panels/HierarchyTree.tsx:61-71,75-97 | medium | isInSubtree/subtreeIds gate every reparent drop, module-private, no test. | Export (or move to lib); HierarchyTree.test.ts: chain ancestry, sibling false, cycle-guard, subtree set. |
| engine/source/saffron/rendering/render_graph.cppm:455-520 | low | CpuScope/GpuScope nesting cursor + cpuMarkerId intern untested. | C++ test: parent indices, depth push/pop, intern stable ids. |
| engine/source/saffron/scene/scene.cppm:417-448 | low | uniqueName collision-suffixing untested. | Assert ' (2)'/' (3)' progression + unchanged base. |
| editor/src/state/dockLayout.ts:355-367,587-590 | medium/low | splitLeaf same-orientation rescale (>=2 children) + validate version!==1 untested. | Add a >=2-child split test asserting ratio sum ~100; a {version:2} -> null test. |
| editor/src/components/dock/useTabStripDrag.ts:158-174 | low | insertionIndexForPointer pure but embedded; untested. | Extract to module scope; assert pinned-skip + end-of-strip. |
| editor/src/control/client.ts:114-130 | low | call() overloads + conditional param-spread wrappers untested. | Bun test mocking invoke: assert request shape + omitted optional keys + string-id contract. |
| editor/src/app/App.tsx:327-366,89-100 | low | Dev-mode 5-click gesture + asset-editor sticky-mount machine untested. | Extract pure advanceDevGesture / nextMountedAssetId; assert thresholds + precedence. |
| editor/src/app/ProjectStartupModal.tsx:260-265 | medium | validProjectName naming contract untested. | Export + test accept/reject cases (length, leading/trailing hyphen, case, space, double-hyphen). |
| editor/src/app/useGizmoShortcuts.ts:42-52 | low | isTextEntryFocused (shared guard) untested. | jsdom test: INPUT/TEXTAREA/SELECT/contentEditable true, button/div false. |
| editor/src/lib/timelineCanvas.ts:73-91,123-133,349-366 | medium | chooseTickStepMs/formatTick/xToSec clamp/withAlpha pure, untested. | Extract to a math module; assert tick step, label formatting, clamp/round-trip, withAlpha expansion. |
| editor/src/components/timeline/TimelineSurface.tsx:66-69,92-114 | low | Track/clip/duration model derived twice with divergent fallbacks. | Centralize deriveModel; feed both canvas and render from it (fixes the divergence too). |
| editor/src/lib/{captureTree.ts,chromeTrace.ts} | low | spansToFlameTree / captureToChromeTrace transforms untested. | profilerTransforms.test.ts: per-lane forests, origin rebase, ms/us conversion, Chrome Trace metadata events. |
| tools/gen-control-dto/gen.ts:1026-1097 | medium | stripComments/parseStructs/parseEnums/validateType (regex C++ parsing) only covered by the CI diff gate, which can't catch a wrong-but-stable parse. | Export + gen.test.ts: field parse, unsupported-member throw, comment strip, nested-type validate, bad-type throw. |
| tools/gen-control-dto/gen.ts:1113-1130,2331-2356 | low | structDeps/transitiveStructs reachability + emitManifest fixture-or-skip guard untested. | Assert transitive closure includes nested struct; DtoTag special-case; emitManifest throws on missing fixture/skip. |
| tools/check-control-schema/check.ts:132-151 | medium | assertRawU64 hardcodes a u64 field-name list that drifts from the DTOs (the exact bug it guards can slip through). | Unit-test the matcher; better, derive the field list from the generated OpenRPC (WireUuid fields). |

## Cross-cutting

### Duplication clusters

- **GPU command-buffer plumbing (engine, ~15+ sites).** The one-off allocate/begin/submit/free dance, the blunt `device.waitIdle()` init tail (12x), the single-mip `transitionImage` re-rolled as local barriers (~6x), and the staging-buffer recipe (3x) are all the *same* idea reimplemented. Land one `withOneOffCommands` + a `transitionImage` subresource overload + `createStagingBuffer` in `renderer_detail.cppm`; this also unifies the two submit-and-wait idioms onto the fence-based path (one-way-to-do-it). The thumbnail offscreen-render scaffold (3x) and the 6 PSO builders (`GraphicsPipelineDesc`) are the next biggest blocks (~400+400 lines).
- **The wire contract is the worst duplication risk.** `gen.ts` is the root cause of an AGENTS.md-documented "four places that must move together" hazard and has already drifted in two independent ways (TS `Material` 8 fields vs 16; `ModelInstance` missing from TS `Components`). Fix order: introduce a single `SCENE_COMPONENTS` data table in gen.ts and derive TS/OpenRPC/C++ serde from it; delete the hand interfaces in `protocol/index.ts` and re-export from `se-types.ts`; the misleading "Produced from the catalog" header on the static `emitSceneSerde` body should be made honest or made true (data-driven).
- **Editor utility duplication.** `errorText` (5 copies, 4 to delete) and `rememberProject` (3 copies) collapse for free once `MenuBar.tsx` is deleted. Then a small set of extractions removes the rest: `useThumbnailUrl`, `InlineRenameInput`, `useFieldCoalescers`, `useDragScrub`+`clamp`, the optimistic-toggle helper, the tree-chrome/`groupByParent` helpers, `base64ToBlob`, `RAD_TO_DEG/DEG_TO_RAD`, `uvToNdc`, `instantiateDroppedModels`, and promoting `guard`/`onError` from timeline/shared.ts into `lib/flash.ts` (27 inline copies).

### Coupling smells

- **Five god-files** (`assets.cppm`, `renderer_detail.cppm`, `renderer.cppm` in C++; `store.ts`, `AssetsPanel.tsx` in TS) hold 5-9 unrelated concerns each and host the engine's largest functions. None require interface changes — the C++ splits move bodies into `module Saffron.X;` impl units (the project's existing pattern), and `store.ts` uses Zustand slices that keep every selector call site byte-identical. These are the highest-leverage structural work and they *unblock testability* (extracted pure helpers become reachable for unit tests).
- **scene_edit_gizmo.cpp violates its own documented invariant** by reading `editor.scene` directly while the sibling `stepEditSmoothing` routes through `activeScene`; during preview the Begin phase checks one scene and snapshots the other. This is the only coupling finding with a behavioral edge (NEEDS-DESIGN: decide whether the gizmo is Edit-only by contract).
- **Panel-to-panel and protocol-in-view edges** (HierarchyTree->InspectorPanel for `orderedComponentNames`; drop targets->AssetTile for the DnD protocol) are one-line moves to neutral `lib/` modules.

### Test-gap plan

There is no C++ unit harness today (only env-gated in-process self-tests + the bun e2e suite), and editor unit tests are thin (a few `*.test.ts` under lib/state). Sequence:

1. **e2e first (no new harness, highest ROI):** asset-folder commands, set-atmosphere, asset-usages — all are currently zero-coverage control commands with real path/merge/walk logic. Add `tests/e2e/asset-folders.test.ts`, `atmosphere.test.ts`, and asset-usages assertions driven over the control plane.
2. **Editor bun tests (harness already exists):** keybindings, frameSeries, materials/graph, store tree helpers, coalesce, perfThresholds, fieldRenderer, the AssetTile/AssetFolderTree pure parsers, graphsEqual, validProjectName. Many require first *extracting* the pure logic out of closures (FrameTimeGraph hysteresis, App gestures, TimelineSurface playhead) — pair these with the relevant STRUCTURAL splits.
3. **C++ pure-logic harness (new):** stand up a small test TU linking the engine (or a test-only exported entry) for the highest-risk pure functions: render_graph barrier derivation, floatToHalf/convertToRgb, SubscriberList, gizmo/clip math, drawlist flatten/stats. These are correctness-critical (a wrong barrier is a data race, not a compile error) and unreachable from the wire.

## Recommended cleanup plan

### Phase 1 — Mechanical, batchable, build-safe (do first, in any order)
- **MECHANICAL:** Delete the ternary operator everywhere it appears in general control flow (the full Conventions table). Collapse repeated forms into helpers as you go: `boolFlag(bool)->u32` (renderer_lighting), `gizmoOpDto/FromDto`+`gizmoSpaceDto/FromDto`+`nativeGizmoHandleName` (scene/sceneedit), `toTrackPath/toTrackInterp` (geometry), `selectorString/optionalFolder` (control_commands_asset). Leave the JSON-boundary cases (assets.cppm 634/654/660/1617/1619) untouched.
- **MECHANICAL:** Remove all banner/divider comments and change-journey phrasing (the full Comments table), including the dead C++ file references in AssetTile/AssetPicker/AssetsPanel/CreateMenu/MenuBar. Fix the geometry `^ u64{0}` NUL-separator comment, reconcile scene.cppm `linear`/Colorspace, add `exposure?` to the set-environment help.
- **MECHANICAL:** Delete dead code: `MenuBar.tsx` (whole file — also kills the errorText/rememberProject dupes), `uploadSvgIcon`, `requestSkyBake`, `asString`/`entityRef`/`renderStatsJson`, `projectInfoJson`/`spawnMesh`, the bake/import wrapper functions in geometry, `assetSlotName`, `card.tsx`, `tabs.tsx`, `leafActiveTab`/`isLeafEmpty`, the three viewport accessors + three renderer getters, `METRICS_WINDOW_LEGACY_KEY` + the stale sidebar comment, the `optionalField`/`cppJsonValue` redundancies in gen.ts, and the unreachable `tabIcon` branches. Confirm-then-cut the uncalled client.ts wrappers (loadProject at minimum).
- **MECHANICAL:** `errorText` consolidation (4 deletions -> import lib/flash), `rememberProject` consolidation (after MenuBar gone), `base64ToBlob`/`RAD_TO_DEG`/`uvToNdc` single-source.
- **NEEDS-DESIGN (small, do here):** Delete the two real compat shims — the `asset_registry.json` migration path in `newAssetServer` (assets.cppm 848-893; verify loadProject/loadCatalog cover the catalog) and the `ListClipsParams::entity` ignored field (update its call site + generator fixture + editor client). These are behavioral so they warrant a deliberate commit, but they are required by the NO-LEGACY rule.

### Phase 2 — Wire-contract consolidation (do before further DTO/component work)
- **NEEDS-DESIGN:** Introduce `SCENE_COMPONENTS` table in `gen.ts` and derive `emitTs`/`componentSchemas`/`emitSceneSerde` from it; fix the already-drifted Material/ModelInstance fields; derive enum tsType unions from `enumWireNames`. **This touches generated output — keep `bun run check`/the control-schema contract test green at each step (the build-green gate is most at risk here).**
- **STRUCTURAL:** Delete the hand interfaces in `protocol/index.ts`; re-export from `se-types.ts`; derive Environment/Selection from DTOs. Make the `emitSceneSerde` header honest or data-driven. Coordinate the `MaterialSlot`/`MaterialComponent` shared-struct extraction (scene.cppm) with this since its serde is generated.

### Phase 3 — Engine duplication helpers (unblocks the function splits)
- **STRUCTURAL:** Land `withOneOffCommands`, the `transitionImage` subresource overload, `createStagingBuffer`, `makeOrderedSetLayout`/`allocateSet`, and `makeBuffer` in `renderer_detail.cppm`; route all ~15 command-buffer sites + barrier lambdas + staging recipes + layout/alloc lambdas through them. Add `GraphicsPipelineDesc`+`buildGraphicsPipeline` and the thumbnail-target helpers. Extract `resetViewImages`/`resetRestirView` (fixes the latent VMA-leak duplication). Add `uuidFromJson`, the `Fnv1a` helper, `compileSlangToSpv`, `quatToEulerZYX`, `decodeAssetSelector`, `requireComponent`, the script `tracebackHandler`/`loadChunkReturningTable` helpers. Each is a green-build-preserving extraction; do these before the large-function splits so the extracted functions are small.

### Phase 4 — Large-function and file splits (sequence so each unblocks the next)
- **STRUCTURAL:** Split the C++ god-functions into the helpers named in the Large-functions table: `initDescriptorResources`, `beginFrameGraph`, `bakeEnvironment`, `submitDrawList`, `renderScene`, `requestThumbnail`, `runHost`, `importGltfModel`, `tickAnimation`. Doing these first makes the subsequent file splits mechanical (functions are already small) and exposes pure helpers for Phase 6 tests.
- **STRUCTURAL:** Then split the god-files by moving bodies into `module Saffron.X;` impl units: `assets.cppm` (thumbnail worker first — only threaded concern), `renderer_detail.cppm`, `renderer.cppm`, `geometry.cppm`, `scene.cppm`, `control_commands_asset.cpp`. **Build-green risk: module BMI churn — build with one ninja per the concurrent-build rule and gate with `make engine` after each file.**
- **STRUCTURAL:** Editor splits: Zustand slices for `store.ts` (metrics+capture first), `AssetsPanel.tsx` into panels/assets/*, `client.ts` into per-domain modules, `ScriptSlots`/`InspectorPanel`/`App.tsx`/`TimelineSurface` extractions. Pull the editor duplication helpers (`useThumbnailUrl`, `InlineRenameInput`, `useFieldCoalescers`, `useDragScrub`, tree-chrome/`groupByParent`, optimistic-toggle, `guard`/`onError` promotion) during these splits.

### Phase 5 — Coupling fixes
- **STRUCTURAL:** Move `orderedComponentNames`+constants to `lib/componentOrder.ts`; extract `lib/assetDnd.ts`; narrow the TimelineSurface store subscription to a selector+equalityFn.
- **NEEDS-DESIGN:** Decide and enforce the gizmo's scene-routing contract (route through `activeScene` or document the Edit-only exemption); fix the Begin-phase cross-scene snapshot.

### Phase 6 — Tests (after the extractions above make logic reachable)
- **STRUCTURAL/NEW:** Add the e2e tests (Phase order: asset-folders, atmosphere, asset-usages — no extraction needed, do these early/independently). Add the editor bun tests against the now-extracted pure helpers. Stand up the C++ unit harness and cover barrier derivation, floatToHalf/convertToRgb, SubscriberList, and the gizmo/clip math.

## Verification notes

These rejected findings were checked and discarded, so the rest can be trusted:

- **assets.cppm:2378-2429 "detectMaterialRole has no test" — rejected.** `tests/e2e/material_import.test.ts` imports rock_diff/_nor/_rough/_disp and asserts the role string covers albedo/normal/roughness/height; `lowerGraphToParams` is covered by material_graph/procedural/codegen tests. "Has no test" was inaccurate (some sub-branches lack direct asserts, but coverage exists).
- **control_commands_render.cpp:266,273 "cross-fade comment restates field name" — rejected.** The cited lines are JSON pushes (`"dur"`, `"correlated"`); the actual cross-fade comment lives in control_commands_animation.cpp:266, and it is a legitimate why-comment.
- **json.cppm:44-45 "jsonF64 lacks an integer-acceptance note" — rejected.** The existing group doc is accurate; this was an additive nice-to-have, not a defect.
- **panelRegistry.tsx:122-125 "change-journey phrasing" — rejected.** "their Scene cousins" describes a current relationship, not a past-tense change.
- **keybindings.ts:1-5 "explains storage by contrast with VS Code" — rejected.** That is a familiar-design analogy for the current scheme, not a contrast with a prior version; the comments rule bans past-contrast, not analogies.
- **tools/se/source/main.cpp:419 "mojibake artifact" — rejected.** `od -tx1` showed clean `e2 80 94` (valid UTF-8 em dashes); the comment intentionally embeds em dashes.

A handful of confirmed findings carried minor factual slips that do not change the verdict and were corrected against the real code: several large-file line counts are off-by-one (trailing newline); `seedEmptyTlas` is at line 860 not 2896 (the command-buffer duplication still holds across 5+ instances); `renderer.cppm:3674-3678` is an if-block not a ternary (the other ~28 sites are real); the `protocol/index.ts` import counts were inflated (Material imported by 1 file, Camera by 0 — but the drift is real); and the `CompatCommandResultOverrides` InspectResult override is *not* redundant (it narrows to the stale hand `Components`), though the finding itself stands.

## Progress log

### Phase 1 — DONE (build-green)

Mechanical cleanup applied across the tree (two rounds, disjoint files per agent), then gated:
engine build ✓, editor typecheck ✓, clang-format (changed files) ✓, oxlint 0 errors ✓, targeted
e2e subset 38/39 ✓.

- **Conventions:** every prohibited ternary in our control flow rewritten to if/else across the
  engine + editor; repeated shapes collapsed into named helpers (`boolFlag`, `gizmoOpDto`/`FromDto`,
  `gizmoSpaceDto`/`FromDto`, `nativeGizmoHandleName`, `toTrackPath`/`toTrackInterp`,
  `selectorString`/`optionalFolder`, `entityIdOrZero`, `lookAtUpForDir`, `resolveTarget`). nlohmann/GLM/vk
  boundary ternaries left as-is. Bare `int`→`i32` in animation. `static`→anon-namespace in scene_edit_gizmo.
- **Comments:** banner/divider + change-journey/migration comments removed or rewritten across engine
  and editor; doc comments referencing deleted DX11-era files rewritten.
- **Dead code:** deleted `MenuBar.tsx`, `ui/card.tsx`, `ui/tabs.tsx`; `projectInfoJson`, `spawnMesh`,
  `assetSlotName`, `uploadSvgIcon`, `requestSkyBake`, `defaultTexture`, `profileCaptureReady`,
  `screenEffectsEnabled`, `viewportImageView`, `viewportGeneration`, `asString`, `entityRef`,
  `renderStatsJson`, `bakeDxToGlNormal`, `bakeGlossToRoughness`, mesh-returning `importGltf`/`importObj`/
  `importModelFile`, file-path `saveMesh`, `runFile`, `leafActiveTab`, `isLeafEmpty`, the
  `useTabStripDrag` `isDraggable` option, the dead timeline `diamonds`/`TimelineKey`/`LaneMode` path, the
  `client.loadProject` wrapper, `METRICS_WINDOW_LEGACY_KEY`. The geometry self-test was rerouted onto the
  live `translateModel`/`saveMeshToBuffer`/`loadMeshFromBytes` path so coverage was preserved.
- **Small dedup:** `errorText` (4 copies → `lib/flash`), `rememberProject` (→ new `lib/recentProjects.ts`),
  `base64ToBlob` (→ store, single source), `RAD_TO_DEG`/`DEG_TO_RAD` (→ `lib/utils.ts`).
- **Compat shim removed:** the `asset_registry.json` one-time migration in `newAssetServer` (verified
  `loadCatalog`/`loadProject` work without it). Docs updated to match.
- **Docs:** updated `renderer-api.md`, `who-can-add-passes.md` (kept), `lua-runtime.md`, `timeline.md`,
  `gltf-and-obj-import.md`, `se-cli-protocol.md`, `project-serialization.md`, `asset-server-and-catalog.md`
  for the removed/renamed symbols.

**Judgment calls made during Phase 1 (review welcome):**

- **`viewportColorResource` RESTORED** after deletion: it is the documented public seam for the
  render-graph "apps add passes via `onRenderGraph`" feature (`who-can-add-passes.md`, rendering
  `AGENTS.md`), not dead code. Kept its decl/def and docs.
- **`client.ts` uncalled wrappers** (`assetReferences`, `cleanAssets`, `deleteUnused`, `getPlayState`,
  `loadScene`, `materialAssign`, `materialImport`, `modelInfo`, `saveScene`, `stopPreview`) LEFT in place
  pending a decision on whether the typed client API is intentionally exhaustive. Only `loadProject`
  (a duplicate of `openProject`) was removed. The underlying control commands stay registered regardless.

### Phase 2 — IN PROGRESS

- **DONE — `protocol/index.ts` drift fix:** deleted the stale hand-declared `Camera`/`Components`/
  `Material`/`Mesh`/`Name`/`Transform`/lights/`ReflectionProbe`/`InspectResult` (the hand `Components` was
  missing `MaterialSet`/`Script`/`Relationship`/`SkinnedMesh`/`Bone`/`FootIk`/`BonePhysics`) and re-exported
  them from the generated `se-types.ts`. Deleted the dead `Protocol` map (0 consumers). Dropped the
  `inspect` override so it uses the full generated `InspectResult`. Kept the genuinely-hand
  `Environment`/`Selection`/`Envelope` (not emitted by the generator).
- **DONE — `Material` drift root cause:** the generated TS `Material` was 8 fields while the OpenRPC
  schema + C++ serde + the real `MaterialComponent` have 16. Fixed `gen.ts`'s `componentInterfaces` to add
  `normalTexture`/`occlusionTexture`/`emissiveTexture`/`heightTexture`/`normalStrength`/`heightScale`/
  `alphaClip`/`alphaCutoff`, so `se-types.ts` `Material` now matches schema + serde. (This was the *actual*
  bug behind the `protocol/index.ts` re-export — the generated type itself was stale. Audited every other
  component for TS-vs-schema drift: `Material` was the only one.)
- **DONE — `gen.ts` cleanup:** removed the redundant `WireUuid` case in `cppJsonValue` (no-op vs `default`);
  made the `emitSceneSerde` header honest (it is hand-maintained, not catalog-derived).
- **DONE — `ListClipsParams::entity` removal:** the "accepted for wire-compat; ignored" field is gone from
  the DTO, help string, generated outputs, and the editor's `listClips()` wrapper (it lied about per-entity
  filtering; the catalog is global). Verified by the animation e2e ("list-clips reports the imported clip").
- All of the above gated green: engine build + `bun run check` + contract test (144) + animation e2e (10/10).
- **REMAINING — deferred with reasoning:**
  - `MaterialSlot`/`MaterialComponent` shared-struct extraction in `scene.cppm` (the `MaterialSet` slot and
    the standalone `MaterialComponent` likely duplicate the 16-field material) — couple with the generated serde.
  - The full `SCENE_COMPONENTS`-table consolidation (derive `componentInterfaces` + `componentSchemas` +
    `emitSceneSerde` from one catalog). **DEFERRED as genuinely NEEDS-DESIGN:** `emitSceneSerde` is highly
    irregular (per-field defaults like `45.0f`/`6360.0f`, struct-field≠wire-key renames like `nearPlane`→`near`,
    per-type helper selection, sub-structs, enums); a byte-identical derivation is high-effort and a subtle
    mistake silently corrupts scene save/load — a poor risk/reward now that the actual drift is fixed and the
    header is honest. If pursued, the committed generated files are the zero-diff oracle (any derivation error
    shows as a diff). A lighter alternative: a codegen-time assertion that the three emitters agree on each
    component's field-key set (drift *detection* without the risky rewrite).

### Phase 4 — IN PROGRESS (file splits)

- **DONE — `assets.cppm` async-thumbnail subsystem → `assets_thumbnail.cpp`** (new `Saffron.Assets` impl unit;
  ~22 functions + the `ThumbnailWorker`/`CachedThumbnail`/`ThumbnailJob` structs moved, exported decls kept in
  `assets.cppm`, wired into `engine/CMakeLists.txt`). `assets.cppm` 6668 → ~5900 lines. Verified: engine build
  + the full thumbnail e2e suite (cache/async/hdr/material/texture/model) green — behavior-preserving.
- **REMAINING:** `renderer_detail.cppm` (5154) + `renderer.cppm` (3868) cohesive-cluster splits (build+e2e
  verifiable), and the editor `store.ts` (Zustand slices) + `AssetsPanel.tsx` (sub-components) splits — the
  editor ones are typecheck/unit-test/lint-gated but NOT UI-verifiable here, so they need the user to run the
  editor (per editor/AGENTS.md "no view of the running editor").

### Phase 6 — LARGELY DONE (tests)

- **17 editor unit-test files added, 363 cases, all green + typecheck-clean.** Batch 1 (already-exported pure
  fns): keybindings, frameSeries, materials/graph, store (buildTree/reanchorPastBones), coalesce,
  perfThresholds, alarmToasts, profilerTransforms. Batch 2 (+ small `export` of module-level pure fns):
  fieldRenderer (inferKind), validProjectName, HierarchyTree (isInSubtree/subtreeIds), AssetFolderTree builders,
  AssetTile DnD parsers, MaterialGraphEditor graphsEqual, FrameTimeGraph niceCeil, timelineCanvas helpers, dock
  splitLeaf. Component-internal logic that wasn't cleanly extractable was skipped (not risk-refactored).
- **3 new e2e tests added & green:** `atmosphere.test.ts` (set-atmosphere merge), `asset-folders.test.ts`
  (create/move/rename-cascade/delete + validation), `asset-usages.test.ts` (mesh + environment.skyTexture slots).
- **REMAINING:** a C++ pure-logic unit harness (barrier derivation, floatToHalf, SubscriberList, gizmo/clip math)
  — the engine has no C++ unit-test infra today; assess standing one up.

### Phase 5 — IN PROGRESS (coupling)

- **DONE — `orderedComponentNames`/`COMPONENT_ORDER`/`HIDDEN_COMPONENTS` → `lib/componentOrder.ts`.** Broke the
  HierarchyTree→InspectorPanel cross-panel import (both now import from the neutral lib module). Pure move, no
  logic change; typecheck-clean + all editor unit tests pass.
- **REMAINING (held — UI-behavioral / NEEDS-DESIGN):** extract the asset DnD protocol to `lib/assetDnd.ts`
  (touches AssetTile + drop targets; DnD is UI-behavioral); narrow the `TimelineSurface` store subscription to a
  selector+equalityFn (UI-behavioral); and the `scene_edit_gizmo.cpp` `editor.scene`-vs-`activeScene` routing
  invariant (NEEDS-DESIGN, behavioral — the Begin phase checks one scene and snapshots the other during preview).
  These need the running editor / deliberate design + gizmo e2e to verify, so they are flagged rather than done
  blind.

### Phase 3 — core done (engine dedup helpers)

- **DONE — latent VMA-leak fix (the correctness item):** the 16-field `ViewTargets` image reset and the
  `RestirView` reset were duplicated verbatim in `destroyRenderer` and `destroyView` (add a per-view image
  to one path but not the other → silent GPU leak). Extracted `resetViewImages(ViewTargets&)` and
  `resetRestirView(RestirView&)` in `renderer.cppm`; both destroy paths now route through one place.
- **DONE — `quatToEulerZYX`:** the `extractEulerAngleZYX(mat4_cast(q), …)` idiom (4 sites across
  `scene.cppm`/`scene_edit_gizmo.cpp`×2/`assets.cppm`) is now one exported `quatToEulerZYX(quat) -> vec3`
  in `Saffron.Scene`. Behavior-identical (the engine's stable Rz*Ry*Rx convention).
- **DONE — `withOneOffCommands`:** added to `renderer_detail.cppm` (alloc a one-off cmd on the one-off pool →
  begin OneTimeSubmit → `record` → end → `submitAndWait` fence → free); routed the two highest-traffic upload
  paths (`uploadMesh`, `uploadTexture`) through it. Behavior-identical — each site keeps its own staging/image
  cleanup around the call and the Err path. Verified: build + clang-format + e2e over the model/texture/
  material/thumbnail/skinning upload paths, validation-clean.
- Gated green: engine build + clang-format + e2e (hierarchy/scene/model_asset/rendering/picking 22/22 plus the
  upload-path suite).
- **REMAINING (GPU-delicate — isolated extraction + rendering/thumbnail e2e + validation-clean each):** route
  the remaining one-off sites through `withOneOffCommands` (the BLAS build, the 2nd texture path, the thumbnail
  scaffolds) — but LEAVE the `frames[0].commandPool`+`device.waitIdle()` *init* tails: that is a separate idiom
  with init-ordering assumptions (the one-off pool may not exist yet), so unifying it is riskier than its value.
  Then a `transitionImage` subresource overload (folds `mipBarrier`/`cubeBarrier`/local barrier lambdas),
  `createStagingBuffer`, the thumbnail offscreen-render scaffold, and `makeBuffer`. Plus the low-risk non-GPU
  ones: `requireComponent` (control_commands_scene, 5 sites — add/remove must NOT check `has`), and the smaller
  helpers (`uuidFromJson`, `Fnv1a`, `compileSlangToSpv`, `decodeAssetSelector`, the script helpers).

**Pre-existing failures found (NOT caused by this cleanup) — two committed e2e tests that the in-flight
asset-container / rig-on-descendant refactor outran, both confirmed HEAD behavior by empirical probes:**

1. `assets.test.ts` → "probe-asset returns on-disk metadata for a mesh": a container-embedded mesh asset's
   path is the `.smodel`, but `probe-asset`→`meshFileCounts` only reads a standalone `.smesh` (rejects `SMDL`
   magic), so `vertexCount` is `undefined`. `probe-asset` isn't container-aware.
2. `skinning.test.ts` → "the skinned mesh resolves its joints by uuid through inspect": a rigged import now
   yields root `skinned-strip` (`ModelInstance`,…) → descendant `SkinnedStrip` that carries the
   `SkinnedMesh`; `instantiate-model` returns the root, so `inspect(root)` has no `SkinnedMesh`. The test
   expects it on the imported id. (My `spawnSkinnedModel` edit only changed `rootBone`'s value — the
   attachment, hierarchy, and return are unchanged HEAD code; the "GPU pass deforms" sibling passes.)

Both are real but out of scope for the cleanup (deferred per the user). Fix #1 by making `probe-asset` read
the `.smodel` mesh chunk; fix #2 by updating the test (or having `inspect`/import resolve to the rig).
