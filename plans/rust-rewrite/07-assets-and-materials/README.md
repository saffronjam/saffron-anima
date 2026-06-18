# 07 — Assets and materials

`saffron-assets` is the orchestration crate that sits on top of geometry (the real importers/byte
codecs), rendering (`GpuMesh`/`GpuTexture` upload + the bindless table + the thumbnail GPU primitives),
and scene (the `AssetCatalog`/`AssetEntry` types and the ECS world). It owns the project's asset
catalog *wrapper* with uuid-keyed GPU caches (the negative-cache), the native `.smat` material system
(factors + textures + instances + node graphs), the node-graph → Slang codegen driven through
`slangc`, the off-thread thumbnail worker, project save/load (`project.json`), the model import/bake
pipeline, and `render_scene` — the engine's single highest-coupling function, which translates a scene
+ camera into renderer draw calls and every per-frame lighting/shadow/GI/sky/RT setter.

The crate is *Hard* not because any one piece is novel but because it cannot be ported in isolation: it
is glue over four other subsystems, and three of its contracts fail silently if they drift — the
negative-cache "a cached null is not a miss" rule, the GPU-resource teardown ordering, and the byte
layout of `.smat` / `project.json` on the frozen wire. The Rust port is genuinely *safer* in exactly
two places, and that safety is the whole point of the rewrite landing here: GPU-resource lifetime
ordering becomes `Arc<GpuMesh>`/`Arc<GpuTexture>` + `Drop` instead of hand-choreographed
`waitGpuIdle`-then-`clearAssetCaches`, and the shell-quoted `slangc` command string becomes a
`std::process::Command` argv (no quoting bug, no `/dev/null 2>&1` redirection hack).

The crate root is `#![deny(unsafe_code)]` — there is no FFI here. The unsafe seams it *touches* (the
ash queue, the bindless table) live in `saffron-rendering` and are reached only through its safe
upload/thumbnail API.

## 1. Crate boundary — what lives here vs. upstream

Three boundaries are decided up front because they shape every phase:

- **The catalog *types* live in `saffron-scene`** (`AssetType`, `Colorspace`, `AssetEntry`,
  `AssetCatalog`, `find_asset`/`put_asset`/`rename_asset`/`unique_name`) — ported by 03-ecs-and-scene
  phase-3. This crate owns the **`AssetServer`** that wraps an `AssetCatalog` with GPU caches, project
  I/O, import, and materials. `AssetServer` is the source of truth for the live catalog; it shares an
  `Arc<AssetCatalog>` into `Scene.catalog` (the scene's `Option<Arc<AssetCatalog>>`, bucket-1 read-share
  per the foundations Ref policy) so `render_scene` and pick can read it without a lifetime tangle.

- **The GPU upload + thumbnail GPU primitives live in `saffron-rendering`** (`upload_mesh`,
  `upload_texture`, `upload_texture_float`, `render_mesh_thumbnail`, `render_material_preview`,
  `bind_thumbnail_worker_thread`, `prewarm_thumbnail_resources`, and the `GpuMesh`/`GpuTexture` move-only
  Drop wrappers — 06-rendering phases 3/5/16). This crate owns the **thumbnail worker thread**, the job
  queue, the image-decode step, the negative-cache, and the cache handback. The two `Arc<Mutex>` GPU
  sites (the graphics queue, the bindless free-list) are owned and exposed by rendering; this crate's
  worker reaches them only by calling rendering's upload functions under rendering's own locking.

- **The byte codecs live in `saffron-geometry`** (`load_mesh_from_bytes`, `save_mesh_to_buffer`,
  `load_anim_clip`, the `.smodel` `ContainerReader`/`write_container`, `decode_image`, `sub_id_for`,
  `translate_model` glTF/OBJ import — 02-math-and-geometry phases 3-7). This crate calls them; it never
  re-implements a byte format. `ContainerMetadata` (the META-chunk record) and the bake/scan logic that
  *uses* the container codec live here.

## 2. The negative-cache (the rule that fails silently)

The cache is the heart of the crate and the explicit feasibility callout: preserve it as
`HashMap<u64, Option<Arc<T>>>`. The semantics, ported verbatim:

- **A present key with `None` is a negative-cache marker, not a miss.** A failed mesh/texture load
  inserts `None` so the asset is *not* retried (or re-warned) every frame. A `get` that finds a key
  returns its `Option<Arc<T>>` as-is (a live `Arc` or the cached `None`); only an *absent* key triggers a
  load attempt. This is the single distinction the C++ encodes with `find() != end()` returning a null
  `Ref`, and Rust expresses cleanly: the outer `Option` (returned by `HashMap::get`) is presence; the
  inner `Option<Arc<T>>` is success-or-negative-cache.

- Three caches, three key spaces, all keyed by `u64` (the uuid/sub-id value):
  `mesh_by_uuid: HashMap<u64, Option<Arc<GpuMesh>>>`,
  `texture_by_uuid: HashMap<u64, Option<Arc<GpuTexture>>>`,
  `model_by_uuid: HashMap<u64, Option<Arc<ModelAsset>>>` (opened `.smodel` containers, by model id).
  Reserved-id sentinels (`DefaultMaterialId{1}`, `PreviewFloorMeshId{2}`, `< 1024` range) are seeded into
  the mesh cache, never the catalog, so a preview-floor mesh renders without a serialized catalog row.

- In the draw path a dangling texture id falls back to rendering's default-white slot (slot 0), *not* a
  load retry — the warn happens once at negative-cache insertion.

This is decided as a tiny generic helper (`fn resolve_cached<T>(cache, key, || load) -> Option<Arc<T>>`)
so the get-or-negative-cache shape is written once, not copy-pasted across `resolve_mesh`,
`resolve_texture`, `load_texture_asset`, `load_mesh_asset`, `load_model_asset`.

## 3. GPU-resource lifetime: `Arc` + `Drop`, idle-before-clear

The C++ rule — "clear caches only after `wait_gpu_idle`" — survives, but as a *call-site discipline*, not
a manual teardown order. `clear_asset_caches` drops the three `HashMap`s; that drops the `Option<Arc<T>>`
values; the last `Arc` drop runs `GpuMesh`/`GpuTexture`'s `Drop`, which frees the VMA allocation and
returns the bindless slot. So `load_project`/`create_project` keep the exact sequence
`wait_gpu_idle(renderer)` → `stop`/`clear_thumbnail_queue` → `clear_asset_caches` → swap the catalog,
because an in-flight frame may still reference an `Arc<GpuTexture>` and dropping it under the GPU is a
use-after-free that `Drop` ordering alone cannot catch (it is a runtime UAF, not a compile error — the
one place this crate must be careful). The worker's un-drained handbacks are also dropped here, safe
because the GPU is idle at the call site.

No `Arc<Mutex>` is needed *in this crate's own state*: the catalog and caches are touched only from the
main thread. The worker thread shares GPU state, but that sharing is mediated by `saffron-rendering`'s
queue/bindless mutexes — see §5.

## 4. Materials: `.smat`, instances, and node-graph folding/codegen

`MaterialAsset` ports as a plain struct (the §1 grounding table lists the field set). Four behaviors are
load-bearing:

- **Instances are parent + sparse overrides.** `parent != 0` resolves to the parent's resolved params
  with this material's `overrides` map applied on top (edit-once-propagate). `0` is a master material.
  Resolution recurses with a depth cap of 8 (cycle/over-deep guard); the resolved result keeps `parent`
  + `overrides` so the editor still sees it as an instance. `DefaultMaterialId{1}` short-circuits to the
  default material.

- **`overrides` and `graph` are opaque JSON.** They are author/editor-shaped trees the engine reads
  field-by-field; they ride as `serde_json::Value` (not a typed struct), exactly as the C++ holds them as
  `nlohmann::json`. `apply_overrides` walks the override map and writes matching `MaterialAsset` fields.

- **Foldable graphs collapse to flat params; non-foldable graphs force codegen.** `lower_graph_to_params`
  walks the graph's `nodes`/`edges`, folds `constant`/`texture` nodes wired to the `materialOutput`
  channels into the flat factor/texture fields, and returns `false` (not foldable) the moment it hits a
  procedural/math node. A non-foldable graph routes to the Slang codegen path (§4 below). This is pure
  CPU JSON-walking and ports 1:1 — but the channel name strings (`baseColor`, `emissive`, `metallic`,
  `roughness`, `emissiveStrength`, `normal`, `height`) and node type strings (`constant`, `texture`,
  `textureSlot`, `materialOutput`) are a frozen contract with the editor's node-graph model and are
  pinned by a fold-correctness test.

- **Texture colorspace is set at import, recovered at resolve.** Albedo/emissive upload sRGB; normal /
  metallic-roughness / occlusion / height upload linear. A standalone file's explicit `.smeta` colorspace
  wins; otherwise the row's `hdr`/`linear` provenance decides; an embedded chunk carries its colorspace
  in the chunk flags. The packed ORM/ARM map feeds *both* the metallic-roughness and occlusion slots.

### Node-graph → Slang codegen via `std::process::Command`

`emit_graph_surface(graph, mesh)` emits the body of `evalSurface` — one Slang statement per node in
array order, then the `materialOutput` channel assignments. Three compile targets, each writing a
`.slang` then invoking `slangc`:

- `compile_material_graph` — a self-contained fragment shader to `materials/<uuid>.spv` (the proof of
  graph→compilable Slang).
- `compile_material_preview_shader` — the studio-lit sphere preview to `materials/<uuid>_preview.spv`.
- `compile_material_mesh_shader` — splices the emitted surface body into the runtime `mesh.slang`
  übershader between the `// @graph-begin` / `// @graph-end` markers, compiles with
  `-I <shaders dir>` so `import lighting` resolves, to `materials/<uuid>_mesh.spv`. `render_scene` points
  a codegen material's `shader` at this `.spv`.

The C++ builds a single `"\"" + slangc + "\" \"" + path + "\" -profile … > /dev/null 2>&1"` string and
runs `std::system`. The Rust port replaces this with `std::process::Command::new(slangc)` and discrete
`.arg(...)` calls (`-profile glsl_450 -target spirv -emit-spirv-directly -fvk-use-entrypoint-name
-matrix-layout-column-major -o <spv>`, plus `-I <dir>` for the mesh variant), `.stdout(Stdio::null())`
`.stderr(Stdio::null())`, and inspects the exit status + the `.spv` existence. **No shell, no quoting,
no redirection string.** This is the one named NO-LEGACY simplification in the area: the hand-quoted
shell command is deleted, not transliterated. `find_slangc` keeps the resolution order
`SAFFRON_SLANGC` env → `~/.cache/saffron-slang/slang/bin/slangc` → `slangc` on `PATH`.

(The xtask shader pipeline in 01-build-and-toolchain compiles the *static* shader set at build time via
its own `slangc` driver; this crate's `slangc` calls are *runtime* per-material compiles into the
project's `assets/materials/` dir. Same tool, different driver, different output dir — they do not share
a code path, and that is correct: the static set is a build artifact, the per-material set is project
content. The argv vector should match the xtask flag set for the overlapping flags.)

## 5. The thumbnail worker (the one cross-thread site)

The worker generates thumbnails off the frame loop so a cold cache-miss never blocks rendering. It is
the only place this crate spawns a thread, and it is *exactly* the marked GPU-queue-sharing site.

- `ThumbnailWorker` becomes a struct owning a `JoinHandle`, a shared job/result state behind a
  `Mutex` + `Condvar` (the C++ `std::mutex` + `std::condition_variable`), and a `stop` flag. The shared
  state holds the job `VecDeque`, the `in_flight`/`failed` dedup `HashSet<String>` (keyed by cache path),
  and the two handback `Vec`s (`(Uuid, Arc<GpuTexture>)` and `(Uuid, Arc<GpuMesh>)`). This is the
  legitimate `Arc<Mutex<WorkerState>>` site — shared mutable across the worker thread and the main
  thread, per the foundations Ref policy bucket 2. The `Arc<GpuTexture>`/`Arc<GpuMesh>` handles in the
  handback are `Send` because the underlying VMA/Vulkan handles are; this is the one place the assets
  crate relies on GPU `Arc`s crossing a thread boundary.

- The worker loop: wait on the condvar for `stop || !queue.empty()`, pop a job, **decode the image bytes
  on the worker thread**, then call rendering's `upload_texture`/`render_material_preview`/
  `render_mesh_thumbnail` (which take the queue + bindless mutexes internally) bound to the worker's
  dedicated command pool via `bind_thumbnail_worker_thread`. Finished `Arc`s push onto the handback;
  failures insert the cache path into `failed` (settle to the type icon). `drain_thumbnail_completions`
  runs once per frame on the main thread, `swap`s the handback vectors out under the lock, and inserts
  the results into the main-thread caches.

- Teardown is sequenced: `stop_thumbnail_worker` sets `stop`, notifies, and joins **before**
  `wait_gpu_idle`/renderer teardown so the worker's last submit's fences have completed and its
  un-handed-back textures are dropped while the renderer is still alive. `clear_thumbnail_queue` (a
  project switch, GPU idle) abandons queued jobs + dedup state + un-drained handbacks; an already-running
  job finishes harmlessly and its single handback is dropped on the next switch.

- The sync fallback (no worker) generates inline on the calling thread, returning the result directly;
  the worker path replies "pending" and the editor polls.

## 6. `render_scene` — the highest-coupling function

`render_scene(renderer, scene, assets, camera, options)` is the orchestrator and stays in this crate
(not rendering), because it reads the scene + the asset caches and drives the renderer. It is one large
function in C++; the Rust port keeps it as one function with extracted helpers (light gather, draw-list
build, shadow/DDGI/sky setup) but does **not** re-architect it into a trait — it is a procedure, and a
procedure is the honest shape. Its responsibilities, each a frozen behavior:

1. Early-out on an invalid camera or a zero-size viewport. Build the Y-flipped `viewProjection`
   (`proj[1][1] *= -1.0` into Vulkan clip).
2. `update_world_transforms(scene)` **once** before any consumer reads — every loop below and the
   between-frame pick/gizmo paths read the `WorldTransformComponent` cache this writes.
3. Gather the first directional light (re-aimed by world rotation if parented), then point + spot lights
   into a `Vec<GpuLight>`, tracking the first spot's perspective light-space transform and the first
   point's position/range for the single shadowed spot/point in v1 (`set_spot_shadow`/`set_point_shadow`).
4. Build the flat `Vec<DrawItem>` from `Transform`+`Mesh` entities (resolving each mesh + its materials
   on demand, accumulating the world AABB + per-draw box proxies for DDGI), then — **gated on
   `skinning_enabled(renderer)`** — the `Transform`+`SkinnedMesh` entities (identity model, joint palette
   built via `joint_matrices`, conservative bind-AABB bounds). Off, the frame is byte-identical to a
   build without the skinned path.
5. Fit the directional shadow ortho frustum to a bounding sphere of the scene AABB
   (`set_directional_shadow`); split the RT scene into static instances (carrying `item.model`) vs.
   skinned (riding the draw list with identity, their deformed verts already world-space) and
   `set_rt_scene`; fit + upload the DDGI volume (`set_ddgi_scene`); snapshot reflection probes
   (consuming each probe's `dirty` flag) and `submit_reflection_probes`.
6. `set_scene_lighting`, drive the environment bake (`request_env_bake` with Equirect/Atmosphere/
   Procedural source resolution from the scene environment + the sun derived from the directional light),
   `set_cluster_camera`, `set_ssao_camera`, `set_show_grid`, optionally append editor-camera models,
   then `submit_draw_list(renderer, viewProjection, items, frame_joints)` and `submit_sky`.

The C++ passes `Renderer&` and calls ~30 free-function setters on it. In Rust these are methods on the
renderer sub-state handles 06-rendering exposes; `render_scene` takes `&mut Renderer` (or the sub-state
borrows the renderer area hands out) + `&Scene` + `&mut AssetServer` (mutable for the on-demand
cache-fill) + `&CameraView`. The borrow shape is: read the scene immutably, fill the asset caches
mutably, push setters + the draw list onto the renderer mutably — three disjoint borrows, legal because
`AssetServer` and `Scene` and `Renderer` are distinct values. `resolve_entity_materials` is the
per-entity helper (Material/MaterialSet/MaterialComponent precedence) that resolves the submesh material
table + the codegen-shader override.

`pick_entity` is the read-side twin: rebuild the same Y-flipped inverse-view-proj, broad-phase per-mesh
AABB then narrow-phase ray-triangle against the CPU mesh copy (static via world matrix, skinned via a
freshly-rebuilt joint palette), return the nearest hit's entity or none. It reads the same last-frame
world-transform flatten as the draw loop (lockstep) but rebuilds the joint palette fresh.

## 7. Import / bake / scan / project I/O

- **`bake_model`** turns an `ImportedModel` (from geometry's `translate_model`) into one self-contained
  `assets/models/<uuid>.smodel`: the mesh chunk, each material as a `.smat`-JSON chunk, each texture as a
  raw chunk (colorspace in the chunk flags), each clip as a `.sanim` chunk, and a META chunk
  (`ContainerMetadata`) carrying the node/skin hierarchy + the deterministic reimport recipe
  (source path, content hash — *not* mtime — `ImporterVersion`, `ImportOptions`). Sub-ids are stable via
  geometry's `sub_id_for`, keyed by source name. `model_id` is reused on reimport (`0` mints a fresh one).
  No GPU, no spawn. `import_model` is the thin wrapper.

- **`scan_assets`/`load_catalog`** make the filesystem the source of truth: walk `assets/` (walkdir),
  reconcile the live catalog against disk (rows added for newly-discovered `.smodel`/standalone files,
  ids removed for vanished files), via a regenerable on-disk catalog cache. `catalog_rows_for_model`
  derives the parent + sub-asset rows from a container's META so a freshly-baked and a rediscovered
  container yield identical rows; the `rigged` flag propagates from the META skin presence, and an
  extracted (remapped) sub-asset points its row at the external file, not the container.

- **`instantiate_model`/`spawn_model`/`spawn_skinned_model`** reconstruct a `ModelSpawnInput` from a
  container's META and spawn entities (soft references: components store sub-ids resolved at draw time).
  The skinned path spawns the node forest + bone entities + the skin descriptor; the root carries a
  `ModelInstanceComponent`. These touch the scene ECS (03-ecs-and-scene), so they take `&mut Scene`.

- **`save_project`/`load_project`/`create_project`** read/write `project.json` (`ProjectVersion = 1`,
  a mismatch is an error). The doc bundles `version` + `name` + `displayName` + `assets`
  (`catalog_to_json`) + `assetFolders` + `scene` (scene's `scene_to_json`) + `renderSettings` + optional
  `editorCamera` + `debugOverlays`. The camera + overlay blocks belong to `saffron-sceneedit`, so they
  ride through `load_project`/`save_project` as opaque `serde_json::Value` round-tripped to the caller
  (the host), never owned here. `load_project` keeps the exact order: parse → version-gate →
  `wait_gpu_idle` → clear worker + caches → set asset root → ensure script `src/` + library → load
  catalog from doc then reconcile against disk → sweep orphan thumbnails → apply render settings →
  pull camera/overlays → `scene_from_json`.

## 8. Errors and the typed surface

The crate exports `pub type Result<T> = std::result::Result<T, Error>` over its own
`#[derive(thiserror::Error)] enum Error` (variants: `Io`, `Json` (`#[from] saffron_json::Error`),
`Geometry` (`#[from] saffron_geometry::Error`), `Render` (`#[from] saffron_rendering::Error`),
`SlangcFailed`, `BadProjectVersion`, `NotInCatalog`, `WrongAssetType`, `ContainerMissingSubAsset`,
`InvalidProjectName`). The C++ `Result<T, std::string>` + `Err("msg")` collapses to `?`; the
negative-cache *load* functions return `Option<Arc<T>>` (presence, not fallibility) and never surface an
`Err` — a failure is a logged warn + a cached `None`, exactly as today. Functions that genuinely fail
(`bake_model`, `load_project`, `compile_material_*`) return `Result`.

## 9. Ref-site ledger (per the foundations Ref policy)

| Site | Bucket | Why |
|---|---|---|
| `mesh_by_uuid` / `texture_by_uuid` / `model_by_uuid` values | `Option<Arc<T>>` (1, + negative cache) | read-shared immutable GPU/container assets; `None` is the negative marker |
| `Scene.catalog` shared from `AssetServer.catalog` | `Arc<AssetCatalog>` (1) | read-shared catalog into the scene; never serialized |
| `editor_camera_model` `SystemMeshVisual.mesh` | `Arc<GpuMesh>` (1) | one shared editor-camera mesh, attempted-once |
| `ThumbnailWorker` shared state (queue/dedup/handback) | `Arc<Mutex<WorkerState>>` (2) | the only cross-thread shared-mutable site; the marked GPU-queue-sharing thread |
| `DrawItem.submesh_materials` `Arc<GpuTexture>` | `Arc<GpuTexture>` (1) | per-frame draw list holds resolved texture handles, all from the cache |

No `Rc<RefCell>` and no `RwLock` in this crate.

## 10. Self-test removal

The C++ in-engine self-tests this crate carries (`runContainerMetadataSelfTest`, the instantiate
self-test stub) are deleted as runtime functions and re-expressed as `#[cfg(test)]` units / fixture
round-trips per the no-self-test rule. The container-metadata round-trip becomes a META encode/decode
golden test; the instantiate self-test becomes a fixture-driven `bake → instantiate → assert entity
shape` integration test.

## Grounding (real files/symbols)

| What | File | Symbols |
|---|---|---|
| `AssetServer` + the three GPU caches + negative-cache rule | `engine-old/source/saffron/assets/assets.cppm` | `AssetServer`, `meshRefByUuid`, `textureRefByUuid`, `modelRefByUuid`, `clearAssetCaches`, `newAssetServer`, `setAssetRoot`, `ensureAssetDirectories` |
| Cache resolve / negative-cache (`null Ref` = marker) | `assets.cppm` | `loadMeshFromSource`, `loadTextureFromSource`, `resolveMesh`, `resolveTexture`, `loadTextureAsset`, `loadMeshAsset`, `loadModelAsset`, `resolveMaterial` |
| `MaterialAsset` + reserved-id sentinels | `assets.cppm` | `MaterialAsset`, `DefaultMaterialId`, `PreviewFloorMeshId`, `defaultMaterialAsset`, `SystemMeshVisual` |
| Material serde + instances + folding | `assets.cppm` | `materialAssetToJson`, `materialAssetFromJson`, `loadMaterialAsset`, `loadMaterialAssetRaw`, `applyOverrides`, `updateMaterialAsset`, `saveMaterialAsset`, `lowerGraphToParams` |
| Node-graph → Slang codegen + `slangc` | `assets.cppm` | `emitGraphSurface`, `findSlangc`, `compileMaterialGraph`, `compileMaterialPreviewShader`, `compileMaterialMeshShader` |
| Material → render-ready submesh material | `assets.cppm` | `buildSubmeshMaterial`, `resolveMaterialAsset`, `resolveEntityMaterials`, `ResolvedMaterials` |
| `render_scene` orchestrator + pick | `assets.cppm` | `renderScene`, `RenderSceneOptions`, `appendEditorCameraModels`, `loadEditorCameraModel`, `ensurePreviewFloorMesh`, `pickEntity`, `entityIdOrZero`, `lookAtUpForDir` |
| Import / bake / scan / instantiate | `assets.cppm` | `bakeModel`, `importModel`, `reimportModel`, `instantiateModel`, `spawnModel`, `spawnSkinnedModel`, `scanAssets`, `loadCatalog`, `catalogRowsForModel`, `ImportOptions`, `BakeResult`, `ScanDelta`, `ImporterVersion` |
| `.smodel` container metadata + model open | `assets.cppm` | `ContainerMetadata`, `encodeContainerMetadata`, `readContainerMetadata`, `ModelAsset`, `loadModelAsset`, `chunkSourceFor`, `ByteSource`, `meshCountsForAsset`, `loadMeshCpuAsset`, `loadAnimationClipAsset` |
| Project I/O | `assets.cppm` | `saveProject`, `loadProject`, `createProject`, `createAutoEmptyProject`, `ProjectVersion`, `ProjectInfo`, `appDataRoot`, `validProjectName`, `defaultDisplayName`, `StarterScript` |
| Texture register / import | `assets.cppm` | `registerTextureBytes`, `registerHdrTextureBytes`, `importTexture`, `importMaterialFolder`, `detectMaterialRole` |
| Thumbnail worker (thread + queue + handback) | `engine-old/source/saffron/assets/assets_thumbnail.cpp` | `ThumbnailWorker`, `ThumbnailJob`, `ThumbnailTextureSource`, `thumbnailWorkerLoop`, `startThumbnailWorker`, `stopThumbnailWorker`, `clearThumbnailQueue`, `drainThumbnailCompletions`, `generateThumbnail`, `thumbnailUploadTexture` |
| Catalog types (upstream, 03-ecs-and-scene) | `engine-old/source/saffron/scene/scene.cppm` | `AssetType`, `Colorspace`, `AssetEntry`, `AssetCatalog`, `findAsset`, `putAsset`, `renameAsset`, `uniqueName` |
| GPU resources + upload + thumbnail GPU (upstream, 06-rendering) | `engine-old/source/saffron/rendering/renderer_types.cppm`, `renderer_drawlist.cpp`, `renderer_textures.cpp`, `renderer_thumbnail.cpp` | `GpuMesh`, `GpuTexture`, `DrawItem`, `SubmeshMaterial`, `uploadMesh`, `uploadTexture`, `uploadTextureFloat`, `submitDrawList`, `renderMeshThumbnail`, `renderMaterialPreview`, `bindThumbnailWorkerThread`, `prewarmThumbnailResources` |

## Phases

1. `phase-1-crate-skeleton-and-asset-server.md` — `saffron-assets` crate + `AssetServer` + the
   negative-cache generic + reserved sentinels + `clear_asset_caches`/Drop ordering.
2. `phase-2-material-asset-and-serde.md` — `MaterialAsset`, `.smat` byte-compatible serde, instances
   (parent + sparse overrides), `default_material_asset`.
3. `phase-3-container-metadata-and-model-open.md` — `ContainerMetadata` META encode/decode,
   `ModelAsset` open (negative-cached), `chunk_source_for` + remap, `ByteSource`.
4. `phase-4-resolve-and-load-paths.md` — the cache resolve/load functions over geometry's codecs +
   rendering's upload (`load_mesh_asset`/`load_texture_asset`/`resolve_*`/`load_anim_clip`/
   `load_mesh_cpu_asset`).
5. `phase-5-node-graph-folding.md` — `lower_graph_to_params` (foldable → flat) + `emit_graph_surface`
   (the Slang surface-body emitter), pure CPU, fully tested without `slangc`.
6. `phase-6-slang-codegen-via-command.md` — `find_slangc` + `compile_material_graph`/`_preview`/`_mesh`
   over `std::process::Command` (the shell-string deletion); the mesh-übershader splice.
7. `phase-7-render-ready-materials.md` — `build_submesh_material`/`resolve_material_asset`/
   `resolve_entity_materials` (Material/MaterialSet/MaterialComponent precedence + codegen override).
8. `phase-8-import-bake-and-scan.md` — `bake_model`/`import_model`/`reimport_model`, `scan_assets`/
   `load_catalog`, `catalog_rows_for_model`, texture register/import, `import_material_folder`.
9. `phase-9-spawn-and-instantiate.md` — `instantiate_model`/`spawn_model`/`spawn_skinned_model` over the
   scene ECS; `ModelSpawnInput`; the node-forest/skin reconstruction.
10. `phase-10-project-io.md` — `save_project`/`load_project`/`create_project`, `ProjectVersion` gate, the
    idle-before-clear sequence, opaque camera/overlay round-trip.
11. `phase-11-thumbnail-worker.md` — the worker thread + `Arc<Mutex<WorkerState>>` + condvar, decode +
    handback + drain, start/stop/clear teardown ordering, the sync fallback.
12. `phase-12-render-scene-and-pick.md` — `render_scene` (the highest-coupling driver) + `pick_entity`,
    the borrow-disjoint three-value shape over the renderer sub-state handles.
