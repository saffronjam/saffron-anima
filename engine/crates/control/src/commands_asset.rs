//! The 51 asset/project-domain control commands: project lifecycle (get/new/open/save/
//! load/reload + create-script), model + texture import + instantiation, the catalog
//! (list/scan/clean/delete-unused), sub-asset extraction, model info + references + the
//! asset model, the asset-preview enter/exit + active-view switch, asset/folder management
//! (rename/move/create-folder/delete), usages + metadata + assignment, the material system
//! (create/assign/import/list/get/update/preview/set-graph/instance/override/compile/cook),
//! scene save+load, screenshot, and thumbnails (get/view/cache).
//!
//! The highest-coupling domain: a handler holds `&mut` to [`AssetServer`][saffron_assets::AssetServer], the
//! [`SceneEditContext`], and the renderer at once through the disjoint
//! [`EngineContext`] fields. Heavy lifting (the importers, the node-graph→Slang codegen,
//! the preview render) lives in `07-assets-and-materials`; these handlers stay thin
//! orchestration. Mirrors `registerAssetCommands` (`control_commands_asset.cpp`).

use std::path::{Path, PathBuf};

use saffron_assets::{
    AssetServer, ContainerMetadata, NewProject, ProjectHost, ProjectInfo, analyze_clean,
    asset_bytes, asset_type_name, build_dependency_graph, clear_extraction, create_project_script,
    default_display_name, default_material_asset, delete_unused, extract_sub_asset,
    import_material_folder, load_material_asset, load_material_asset_raw, lower_graph_to_params,
    reimport_model, request_thumbnail, save_material_asset, update_material_asset,
    valid_project_name,
};
use saffron_core::Uuid;
use saffron_protocol::{
    AnimationClipDto, AssetCapabilitiesDto, AssetEntryDto, AssetList, AssetMetadataDto,
    AssetMetadataParams, AssetModelResult, AssetRef, AssetReferencesParams, AssetReferencesResult,
    AssetSlotDto, AssetTypeDto, AssetUsageDto, AssetUsagesParams, AssetUsagesResult,
    AssignAssetParams, AssignAssetResult, BoneDto, CleanAssetsParams, CleanCandidateDto,
    CleanReport, ClearExtractionParams, CreateAssetFolderParams, CreateScriptParams,
    CreateScriptResult, DeleteAssetFolderParams, DeleteAssetParams, DeleteAssetResult,
    DeleteUnusedParams, DeleteUnusedResult, EmptyParams, EntityRef, ExtractSubAssetParams,
    GetAssetModelParams, ImportModelResult, ImportTextureResult, InstantiateModelParams,
    MaterialAssignParams, MaterialAssignResult, MaterialCompileParams, MaterialCompileResult,
    MaterialCookResult, MaterialCreateInstanceParams, MaterialCreateParams, MaterialCreateResult,
    MaterialGetParams, MaterialGetResult, MaterialImportParams, MaterialImportResultDto,
    MaterialListResult, MaterialRefDto, MaterialSetGraphParams, MaterialSetGraphResult,
    MaterialSetOverrideParams, MaterialSetOverrideResult, MaterialUpdateParams,
    MaterialUpdateResult, ModelInfoParams, ModelInfoResult, ModelSubAssetDto, MoveAssetParams,
    NewProjectParams, OptionalPathParams, PathParams, PathResult, PlayStateResult,
    PreviewRenderParams, PreviewRenderResult, ProjectInfoDto, QuitResult, ReimportModelParams,
    ReimportModelResult, RenameAssetFolderParams, RenameAssetParams, ScanAssetsResult,
    ScreenshotParams, ScreenshotResult, ScreenshotTargetDto, SetActiveViewParams,
    SetActiveViewResult, ThumbnailCacheParams, ThumbnailCacheResult, ThumbnailParams,
    ThumbnailResult, Uuid as WireUuid, Vec3, Vec4,
};
use saffron_rendering::{PngTransfer, ViewId};
use saffron_scene::{
    AnimationPlayer, AssetEntry, AssetType, DirectionalLight, Entity, IdComponent, Material,
    MaterialAsset as MaterialAssetComponent, Mesh, Name, Scene, SkinnedMesh, SkyMode, Transform,
};
use saffron_sceneedit::{PlayState, SceneEditCamera};
use serde_json::{Value, json};

use crate::error::{Error, Result};
use crate::registry::{CommandRegistry, ControlRenderer, EngineContext};
use crate::selector::{entity_ref_dto, entity_uuid, resolve_entity};

/// The `base64` standard encoder (the C++ `base64Encode`), used by `preview-render` and
/// the thumbnail commands to ship PNG bytes inside a JSON string.
fn base64_encode(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = *chunk.get(1).unwrap_or(&0) as u32;
        let b2 = *chunk.get(2).unwrap_or(&0) as u32;
        let triple = (b0 << 16) | (b1 << 8) | b2;
        out.push(ALPHABET[(triple >> 18) as usize & 0x3f] as char);
        out.push(ALPHABET[(triple >> 12) as usize & 0x3f] as char);
        out.push(if chunk.len() > 1 {
            ALPHABET[(triple >> 6) as usize & 0x3f] as char
        } else {
            '='
        });
        out.push(if chunk.len() > 2 {
            ALPHABET[triple as usize & 0x3f] as char
        } else {
            '='
        });
    }
    out
}

/// The wire `AssetTypeDto` for a catalog kind (the C++ `assetTypeDto`).
fn asset_type_dto(asset_type: AssetType) -> AssetTypeDto {
    match asset_type {
        AssetType::Texture => AssetTypeDto::Texture,
        AssetType::Other => AssetTypeDto::Other,
        AssetType::Animation => AssetTypeDto::Animation,
        AssetType::Material => AssetTypeDto::Material,
        AssetType::Model => AssetTypeDto::Model,
        AssetType::Mesh => AssetTypeDto::Mesh,
    }
}

/// Reads an id-or-name selector value as its string form, treating any non-string as empty
/// (the C++ `selectorString`).
fn selector_string(selector: &Value) -> String {
    selector.as_str().unwrap_or_default().to_owned()
}

/// Wraps a folder path as an optional, mapping an empty path to `None` (the C++
/// `optionalFolder`).
fn optional_folder(folder: &str) -> Option<String> {
    if folder.is_empty() {
        None
    } else {
        Some(folder.to_owned())
    }
}

/// The uuid an id-or-name selector resolves to: an unsigned number, a non-negative signed
/// number, or a whole-string decimal parse (the C++ `resolveAsset`'s `byId` decode).
fn selector_id(selector: &Value) -> u64 {
    if let Some(value) = selector.as_u64() {
        return value;
    }
    if let Some(value) = selector.as_i64() {
        return u64::try_from(value).unwrap_or(0);
    }
    selector_string(selector).parse::<u64>().unwrap_or(0)
}

/// Resolves an [`AssetSelector`](saffron_protocol::AssetSelector) to a catalog entry id
/// (the C++ `resolveAsset` — by id or name).
fn resolve_asset(ctx: &EngineContext<'_>, selector: &Value) -> Result<Uuid> {
    let by_id = selector_id(selector);
    let name = selector_string(selector);
    for entry in &ctx.assets.catalog.entries {
        if entry.id.value() == by_id || entry.name == name {
            return Ok(entry.id);
        }
    }
    Err(Error::command(format!("no asset '{name}'")))
}

/// Resolves an [`AssetSelector`](saffron_protocol::AssetSelector) to its index in the
/// catalog `entries` (the C++ `resolveAssetIndex`).
fn resolve_asset_index(ctx: &EngineContext<'_>, selector: &Value) -> Result<usize> {
    let by_id = selector_id(selector);
    let name = selector_string(selector);
    for (i, entry) in ctx.assets.catalog.entries.iter().enumerate() {
        if entry.id.value() == by_id || entry.name == name {
            return Ok(i);
        }
    }
    Err(Error::command(format!("no asset '{name}'")))
}

/// Rebuilds the catalog's `by_id` index after an in-place `entries` mutation (the C++
/// `rebuildAssetIndex`).
fn rebuild_asset_index(catalog: &mut saffron_scene::AssetCatalog) {
    catalog.by_id.clear();
    for (i, entry) in catalog.entries.iter().enumerate() {
        catalog.by_id.insert(entry.id.value(), i);
    }
}

/// The wire DTO for one catalog entry (the C++ `assetDto`).
fn asset_dto(entry: &AssetEntry) -> AssetEntryDto {
    AssetEntryDto {
        id: WireUuid(entry.id.value()),
        name: entry.name.clone(),
        r#type: asset_type_dto(entry.asset_type),
        path: entry.path.clone(),
        folder: optional_folder(&entry.folder),
        container: (entry.container.value() != 0).then(|| WireUuid(entry.container.value())),
        duration: (entry.asset_type == AssetType::Animation).then_some(entry.duration),
        rigged: entry.rigged.then_some(true),
    }
}

/// The compact `{ id, name, folder? }` reply for a catalog entry (the C++ `assetRef`).
fn asset_ref(entry: &AssetEntry) -> AssetRef {
    AssetRef {
        id: WireUuid(entry.id.value()),
        name: entry.name.clone(),
        folder: optional_folder(&entry.folder),
    }
}

/// The full catalog as an [`AssetList`] (the C++ `assetListDto`).
fn asset_list_dto(catalog: &saffron_scene::AssetCatalog) -> AssetList {
    AssetList {
        assets: catalog.entries.iter().map(asset_dto).collect(),
        folders: catalog.folders.clone(),
    }
}

/// Whether a folder path is well-formed: non-empty, no leading/trailing `/`, no `\`, and no
/// empty `//` segment (the C++ `validFolderPath`).
fn valid_folder_path(folder: &str) -> bool {
    if folder.is_empty()
        || folder.starts_with('/')
        || folder.ends_with('/')
        || folder.contains('\\')
        || folder.contains("//")
    {
        return false;
    }
    true
}

/// Whether the catalog already carries `folder` (the C++ `hasFolder`).
fn has_folder(catalog: &saffron_scene::AssetCatalog, folder: &str) -> bool {
    catalog.folders.iter().any(|existing| existing == folder)
}

/// Whether `candidate` is a strict descendant folder of `folder` (the C++
/// `isFolderDescendant`).
fn is_folder_descendant(candidate: &str, folder: &str) -> bool {
    candidate.len() > folder.len()
        && candidate.starts_with(folder)
        && candidate.as_bytes()[folder.len()] == b'/'
}

/// Re-roots `value` from the `from` folder prefix onto `to` (the C++ `replaceFolderPrefix`).
fn replace_folder_prefix(value: &str, from: &str, to: &str) -> String {
    if value == from {
        return to.to_owned();
    }
    if is_folder_descendant(value, from) {
        return format!("{to}{}", &value[from.len()..]);
    }
    value.to_owned()
}

/// The entity's `Name`, or empty (the C++ `entityName`).
fn entity_name(scene: &Scene, entity: Entity) -> String {
    scene
        .with_component::<Name, _>(entity, |n| n.name.clone())
        .unwrap_or_default()
}

/// The entity's `IdComponent` uuid as an optional wire uuid (the C++ `entityId`).
fn entity_id(scene: &Scene, entity: Entity) -> Option<WireUuid> {
    let id = entity_uuid(scene, entity);
    (id != 0).then_some(WireUuid(id))
}

/// One `(entity, slot)` reference, collected during a scene scan and resolved to a usage DTO
/// after — so the scan's `&mut` scene borrow does not overlap the per-entity name/id reads.
type Reference = (Entity, &'static str);

/// Collects every `(entity, slot)` reference to `asset` in the scene (the scan half of
/// [`collect_asset_usages`] / [`clear_asset_usages`]): mesh slots + material albedo /
/// metallic-roughness slots. The environment sky-texture hit is the boolean second tuple.
fn scan_asset_references(scene: &mut Scene, asset: Uuid) -> (Vec<Reference>, bool) {
    let mut refs = Vec::new();
    scene.for_each::<(&Mesh,), _>(|entity, (mesh,)| {
        if mesh.mesh.value() == asset.value() {
            refs.push((entity, "mesh"));
        }
    });
    scene.for_each::<(&Material,), _>(|entity, (material,)| {
        if material.albedo_texture.value() == asset.value() {
            refs.push((entity, "albedo"));
        }
        if material.metallic_roughness_texture.value() == asset.value() {
            refs.push((entity, "metallic-roughness"));
        }
    });
    let sky = scene.environment.sky_texture.value() == asset.value();
    (refs, sky)
}

/// One collected `(entity, slot)` reference as a usage DTO (the name/id read after the scan).
fn usage_dto(scene: &Scene, entity: Entity, slot: &str) -> AssetUsageDto {
    AssetUsageDto {
        entity: entity_id(scene, entity),
        entity_name: Some(entity_name(scene, entity)),
        slot: slot.to_owned(),
    }
}

/// Every place `asset` is referenced in the active scene (the C++ `collectAssetUsages`):
/// mesh slots, material albedo / metallic-roughness slots, and the environment sky texture.
fn collect_asset_usages(scene: &mut Scene, asset: Uuid) -> Vec<AssetUsageDto> {
    let (refs, sky) = scan_asset_references(scene, asset);
    let mut usages: Vec<AssetUsageDto> = refs
        .iter()
        .map(|&(entity, slot)| usage_dto(scene, entity, slot))
        .collect();
    if sky {
        usages.push(AssetUsageDto {
            entity: None,
            entity_name: None,
            slot: "environment.skyTexture".to_owned(),
        });
    }
    usages
}

/// Clears every reference to `asset` in the scene and returns the cleared usages (the C++
/// `clearAssetUsages`, the `delete-asset` cascade). The DTOs are built (name/id read) before
/// the slot is zeroed.
fn clear_asset_usages(scene: &mut Scene, asset: Uuid) -> Vec<AssetUsageDto> {
    let (refs, sky) = scan_asset_references(scene, asset);
    let mut cleared: Vec<AssetUsageDto> = refs
        .iter()
        .map(|&(entity, slot)| usage_dto(scene, entity, slot))
        .collect();
    for &(entity, slot) in &refs {
        match slot {
            "mesh" => {
                let _ = scene.with_component_mut::<Mesh, _>(entity, |m| m.mesh = Uuid(0));
            }
            "albedo" => {
                let _ =
                    scene.with_component_mut::<Material, _>(entity, |m| m.albedo_texture = Uuid(0));
            }
            _ => {
                let _ = scene.with_component_mut::<Material, _>(entity, |m| {
                    m.metallic_roughness_texture = Uuid(0);
                });
            }
        }
    }
    if sky {
        cleared.push(AssetUsageDto {
            entity: None,
            entity_name: None,
            slot: "environment.skyTexture".to_owned(),
        });
        scene.environment.sky_texture = Uuid(0);
    }
    cleared
}

/// The current project's identity, read from the editor (the C++ `currentProjectInfo`).
fn current_project_info(ctx: &EngineContext<'_>) -> ProjectInfo {
    ProjectInfo {
        loaded: ctx.scene_edit.project_loaded,
        root: ctx.scene_edit.project_root.clone(),
        path: ctx.scene_edit.project_path.clone(),
        name: ctx.scene_edit.project_name.clone(),
        display_name: ctx.scene_edit.project_display_name.clone(),
    }
}

/// Writes a [`ProjectInfo`] back onto the editor, also resetting the scene path (the C++
/// `applyProjectInfo`).
fn apply_project_info(ctx: &mut EngineContext<'_>, project: &ProjectInfo) {
    ctx.scene_edit.project_loaded = project.loaded;
    ctx.scene_edit.project_root = project.root.clone();
    ctx.scene_edit.project_path = project.path.clone();
    ctx.scene_edit.project_name = project.name.clone();
    ctx.scene_edit.project_display_name = project.display_name.clone();
    ctx.scene_edit.scene_path = project.path.clone();
}

/// Brings the host's project up from the editor-set environment at startup (the C++ host's
/// `config.onCreate` project block, `host.cppm:1355`): `SAFFRON_PROJECT` selects a project
/// to open (or create when the name is valid and unborn), else `SAFFRON_AUTO_EMPTY_PROJECT`
/// makes a deterministic per-shell scratch project, else a `project.json` in the working
/// directory is opened. With none of those set the host waits for the editor's project
/// picker. Drives the same [`AssetServer`] + [`apply_project_info`] path the lifecycle
/// commands use, so there is one project-bring-up code path. Failures are logged, not fatal.
pub fn bootstrap_project_from_env(ctx: &mut EngineContext<'_>) {
    let defs = ctx.renderer.sa_lua_defs();

    if let Some(selected) = std::env::var_os("SAFFRON_PROJECT")
        .map(|v| v.to_string_lossy().into_owned())
        .filter(|s| !s.is_empty())
    {
        let mut project = ProjectInfo::default();
        let create_new =
            valid_project_name(&selected) && !saffron_assets::project_json_path(&selected).exists();
        let mut host = RendererProjectHost {
            renderer: ctx.renderer,
        };
        let outcome = if create_new {
            let spec = NewProject {
                name: selected.clone(),
                display_name: String::new(),
                root: String::new(),
            };
            ctx.assets
                .create_project(
                    &mut host,
                    &ctx.scene_edit.registry,
                    &mut ctx.scene_edit.scene,
                    &mut project,
                    &spec,
                    &defs,
                )
                .map(|()| None)
        } else {
            ctx.assets
                .load_project(
                    &mut host,
                    &ctx.scene_edit.registry,
                    &mut ctx.scene_edit.scene,
                    &mut project,
                    &selected,
                    &defs,
                )
                .map(Some)
        };
        match outcome {
            Ok(sidecar) => apply_loaded_project(ctx, &project, sidecar.as_ref()),
            Err(err) => saffron_core::log_error!("project bring-up: {err}"),
        }
        return;
    }

    if std::env::var_os("SAFFRON_AUTO_EMPTY_PROJECT").is_some() {
        let mut project = ProjectInfo::default();
        let mut host = RendererProjectHost {
            renderer: ctx.renderer,
        };
        match ctx.assets.create_auto_empty_project(
            &mut host,
            &ctx.scene_edit.registry,
            &mut ctx.scene_edit.scene,
            &mut project,
            &defs,
        ) {
            Ok(()) => apply_loaded_project(ctx, &project, None),
            Err(err) => saffron_core::log_error!("auto-empty project bring-up: {err}"),
        }
        return;
    }

    let default_project = Path::new("project.json");
    if default_project.exists() {
        let mut project = ProjectInfo::default();
        let mut host = RendererProjectHost {
            renderer: ctx.renderer,
        };
        match ctx.assets.load_project(
            &mut host,
            &ctx.scene_edit.registry,
            &mut ctx.scene_edit.scene,
            &mut project,
            "project.json",
            &defs,
        ) {
            Ok(sidecar) => apply_loaded_project(ctx, &project, Some(&sidecar)),
            Err(err) => saffron_core::log_error!("default project bring-up: {err}"),
        }
    }
}

/// Applies a freshly created/opened project to the live editor state: the project metadata,
/// the (optional) loaded sidecar camera + debug overlays, the scene-version bump, and the
/// reset selection/script-input — the shared tail of the project bring-up + the
/// `open-project` command.
fn apply_loaded_project(
    ctx: &mut EngineContext<'_>,
    project: &ProjectInfo,
    sidecar: Option<&saffron_assets::ProjectSidecar>,
) {
    apply_project_info(ctx, project);
    if let Some(sidecar) = sidecar {
        ctx.scene_edit.camera.from_json(&sidecar.editor_camera);
        saffron_sceneedit::debug_overlays_from_json(
            &mut ctx.scene_edit.debug_overlays,
            &sidecar.debug_overlays,
        );
    }
    ctx.scene_edit.scene_version += 1;
    ctx.scene_edit.script_input = saffron_scene::ScriptInputState::default();
    ctx.scene_edit.set_selection(Entity::NULL);
}

/// The wire DTO for a [`ProjectInfo`] (the C++ `projectDto`).
fn project_dto(project: &ProjectInfo) -> ProjectInfoDto {
    ProjectInfoDto {
        loaded: project.loaded,
        root: project.root.clone(),
        path: project.path.clone(),
        name: project.name.clone(),
        display_name: project.display_name.clone(),
    }
}

/// The "no project loaded" guard (the C++ `requireProjectLoaded`).
fn require_project_loaded(ctx: &EngineContext<'_>) -> Result<()> {
    if ctx.scene_edit.project_loaded {
        Ok(())
    } else {
        Err(Error::command("no project loaded"))
    }
}

/// The [`ProjectHost`] adapter over the renderer seam: the project lifecycle commands hand
/// it to [`AssetServer::create_project`][saffron_assets::AssetServer::create_project] /
/// `load_project` / `save_project`, which need the
/// GPU-idle + render-settings serde the renderer owns. Wraps `&mut dyn ControlRenderer`,
/// so it borrows only the renderer field of the [`EngineContext`] — disjoint from `assets`
/// and `scene_edit`.
struct RendererProjectHost<'a> {
    renderer: &'a mut dyn ControlRenderer,
}

impl ProjectHost for RendererProjectHost<'_> {
    fn wait_gpu_idle(&mut self) {
        self.renderer.wait_gpu_idle();
    }

    fn render_settings_to_json(&self) -> Value {
        self.renderer.render_settings_to_json()
    }

    fn apply_render_settings(&mut self, settings: &Value) {
        self.renderer.apply_render_settings(settings);
    }
}

/// Resolves `{asset, size?}` to a base64-PNG thumbnail reply, driving
/// [`request_thumbnail`] through the renderer's [`ThumbnailGpu`](saffron_assets::ThumbnailGpu)
/// seam. A cache hit returns the PNG; a cold miss replies `pending` (the C++
/// `thumbnailResult`). Shared by `get-thumbnail` (128) + `view-asset` (512).
fn thumbnail_result(
    ctx: &mut EngineContext<'_>,
    params: &ThumbnailParams,
    default_size: u32,
) -> Result<ThumbnailResult> {
    let id = resolve_asset(ctx, &params.asset)?;
    let size = u32::try_from(params.size.unwrap_or(default_size as i32)).unwrap_or(default_size);
    let assets = &mut *ctx.assets;
    let mut reply = None;
    ctx.renderer.with_thumbnail_gpu(&mut |gpu| {
        reply = Some(request_thumbnail(assets, gpu, id, size));
    });
    let reply = reply
        .ok_or_else(|| Error::command("thumbnail seam unavailable"))?
        .map_err(|e| Error::command(e.to_string()))?;
    if reply.pending {
        return Ok(ThumbnailResult {
            id: WireUuid(id.value()),
            format: "png".to_owned(),
            width: 0,
            height: 0,
            base64: String::new(),
            pending: true,
        });
    }
    Ok(ThumbnailResult {
        id: WireUuid(id.value()),
        format: "png".to_owned(),
        width: i32::try_from(reply.width).unwrap_or(0),
        height: i32::try_from(reply.height).unwrap_or(0),
        base64: base64_encode(&reply.png),
        pending: false,
    })
}

/// Drops the asset preview and restores the authored edit state (the C++
/// `leaveAssetPreview`). A no-op when no preview is alive.
fn leave_asset_preview(ctx: &mut saffron_sceneedit::SceneEditContext) {
    if ctx.preview_scene.is_none() {
        return;
    }
    let was_active = ctx.preview_active_view;
    ctx.preview_scene = None;
    ctx.preview_asset = Uuid(0);
    ctx.preview_root_entity = Entity::NULL;
    ctx.preview_floor_entity = Entity::NULL;
    ctx.preview_bone_by_node.clear();
    ctx.preview_active_view = false;
    if was_active {
        ctx.camera = ctx.saved_camera;
        ctx.skeleton_overlay = ctx.saved_overlay;
        let restore = if ctx.saved_selection != Entity::NULL && ctx.scene.valid(ctx.saved_selection)
        {
            ctx.saved_selection
        } else {
            Entity::NULL
        };
        ctx.saved_selection = Entity::NULL;
        ctx.set_selection(restore);
    }
    ctx.scene_version += 1;
    ctx.animation_version += 1;
}

/// Parks the preview orbit and restores the authored fly-cam/overlay/selection so the scene
/// view shows the authored scene (the C++ `deactivatePreviewView`). A no-op unless the
/// preview is the active view.
fn deactivate_preview_view(ctx: &mut saffron_sceneedit::SceneEditContext) {
    if ctx.preview_scene.is_none() || !ctx.preview_active_view {
        return;
    }
    ctx.parked_preview_camera = ctx.camera;
    ctx.camera = ctx.saved_camera;
    ctx.skeleton_overlay = ctx.saved_overlay;
    let restore = if ctx.saved_selection != Entity::NULL && ctx.scene.valid(ctx.saved_selection) {
        ctx.saved_selection
    } else {
        Entity::NULL
    };
    ctx.set_selection(restore);
    ctx.preview_active_view = false;
    ctx.scene_version += 1;
    ctx.animation_version += 1;
}

/// Re-stashes the authored view and restores the parked preview orbit + overlay + selected
/// root (the C++ `activatePreviewView`). A no-op unless a preview scene is alive but not
/// currently active.
fn activate_preview_view(ctx: &mut saffron_sceneedit::SceneEditContext) {
    if ctx.preview_scene.is_none() || ctx.preview_active_view {
        return;
    }
    ctx.saved_camera = ctx.camera;
    ctx.saved_selection = ctx.selected;
    ctx.saved_overlay = ctx.skeleton_overlay;
    ctx.camera = ctx.parked_preview_camera;
    ctx.skeleton_overlay.show = true;
    ctx.skeleton_overlay.highlight_joint = -1;
    ctx.preview_active_view = true;
    let root = ctx.preview_root_entity;
    ctx.set_selection(root);
    ctx.scene_version += 1;
    ctx.animation_version += 1;
}

/// The flat parent-indexed bone tree for a skinned container's nodes (the C++
/// `get-asset-model` rig walk): the joints plus their ancestor chains bounded at the
/// skeleton root, with node indices preserved.
fn build_bone_tree(meta: &ContainerMetadata) -> Vec<BoneDto> {
    let node_count = meta.nodes.as_array().map_or(0, Vec::len);
    let nodes = meta.nodes.as_array();
    let parents: Vec<i32> = (0..node_count)
        .map(|i| {
            nodes
                .and_then(|n| n[i].get("parent"))
                .and_then(Value::as_i64)
                .map_or(-1, |v| v as i32)
        })
        .collect();
    let mut is_joint = vec![false; node_count];
    let skeleton_root = meta
        .skin
        .get("skeletonRoot")
        .and_then(Value::as_i64)
        .map_or(-1, |v| v as i32);
    if let Some(joints) = meta.skin.get("joints").and_then(Value::as_array) {
        for joint in joints {
            if let Some(index) = joint.as_i64()
                && index >= 0
                && (index as usize) < node_count
            {
                is_joint[index as usize] = true;
            }
        }
    }
    let mut in_rig = vec![false; node_count];
    if skeleton_root >= 0 && (skeleton_root as usize) < node_count {
        in_rig[skeleton_root as usize] = true;
    }
    for start in is_joint
        .iter()
        .enumerate()
        .filter_map(|(i, &joint)| joint.then_some(i as i32))
    {
        let mut node = start;
        while node >= 0 && (node as usize) < node_count && !in_rig[node as usize] {
            in_rig[node as usize] = true;
            if node == skeleton_root {
                break;
            }
            node = parents[node as usize];
        }
    }
    let mut bones = Vec::new();
    for i in 0..node_count {
        if !in_rig[i] {
            continue;
        }
        let parent = parents[i];
        let parent = if parent >= 0 && (parent as usize) < node_count && in_rig[parent as usize] {
            parent
        } else {
            -1
        };
        bones.push(BoneDto {
            index: i as i32,
            name: nodes
                .and_then(|n| n[i].get("name"))
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_owned(),
            parent,
            joint: is_joint[i],
        });
    }
    bones
}

/// The animation sub-asset clips a container carries (the C++ `get-asset-model` clip walk).
fn container_clips(meta: &ContainerMetadata) -> Vec<AnimationClipDto> {
    meta.sub_assets
        .iter()
        .filter(|sub| sub.asset_type == AssetType::Animation)
        .map(|sub| AnimationClipDto {
            id: WireUuid(sub.sub_id.value()),
            name: sub.name.clone(),
            duration: sub.duration,
            tracks: sub.tracks,
        })
        .collect()
}

/// Registers the asset/project-domain commands in the frozen manifest order
/// (`get-project` … `quit`).
pub fn register_asset_commands(reg: &mut CommandRegistry) {
    reg.register::<EmptyParams, ProjectInfoDto>(
        "get-project",
        "get-project — active project metadata",
        |ctx, _params| Ok(project_dto(&current_project_info(ctx))),
    );

    reg.register::<NewProjectParams, ProjectInfoDto>(
        "new-project",
        "new-project {name, displayName?, root?}",
        |ctx, params| {
            if ctx.scene_edit.play_state != PlayState::Edit {
                return Err(Error::command("stop play first"));
            }
            if ctx.scene_edit.previewing() {
                return Err(Error::command("exit the asset preview first"));
            }
            let spec = NewProject {
                name: params.name.unwrap_or_default(),
                display_name: params.display_name.unwrap_or_default(),
                root: params.root.unwrap_or_default(),
            };
            let defs = ctx.renderer.sa_lua_defs();
            let mut project = ProjectInfo::default();
            let mut host = RendererProjectHost {
                renderer: ctx.renderer,
            };
            ctx.assets
                .create_project(
                    &mut host,
                    &ctx.scene_edit.registry,
                    &mut ctx.scene_edit.scene,
                    &mut project,
                    &spec,
                    &defs,
                )
                .map_err(|e| Error::command(e.to_string()))?;
            apply_project_info(ctx, &project);
            ctx.scene_edit.scene_version += 1;
            ctx.scene_edit.script_input = saffron_scene::ScriptInputState::default();
            ctx.scene_edit.set_selection(Entity::NULL);
            Ok(project_dto(&project))
        },
    );

    reg.register::<CreateScriptParams, CreateScriptResult>(
        "create-script",
        "create-script {name} — boilerplate .lua under the project src/",
        |ctx, params| {
            if !ctx.scene_edit.project_loaded {
                return Err(Error::command("no project loaded"));
            }
            let path = create_project_script(&ctx.scene_edit.project_root, &params.name)
                .map_err(|e| Error::command(e.to_string()))?;
            Ok(CreateScriptResult { path })
        },
    );

    reg.register::<PathParams, ProjectInfoDto>(
        "open-project",
        "open-project {path}",
        |ctx, params| {
            if ctx.scene_edit.play_state != PlayState::Edit {
                return Err(Error::command("stop play first"));
            }
            if ctx.scene_edit.previewing() {
                return Err(Error::command("exit the asset preview first"));
            }
            if params.path.is_empty() {
                return Err(Error::command("missing 'path'"));
            }
            let defs = ctx.renderer.sa_lua_defs();
            let mut project = ProjectInfo::default();
            let mut host = RendererProjectHost {
                renderer: ctx.renderer,
            };
            let sidecar = ctx
                .assets
                .load_project(
                    &mut host,
                    &ctx.scene_edit.registry,
                    &mut ctx.scene_edit.scene,
                    &mut project,
                    &params.path,
                    &defs,
                )
                .map_err(|e| Error::command(e.to_string()))?;
            apply_loaded_project(ctx, &project, Some(&sidecar));
            Ok(project_dto(&project))
        },
    );

    reg.register::<PathParams, ImportModelResult>(
        "import-model",
        "import-model {path}",
        |ctx, params| {
            if params.path.is_empty() {
                return Err(Error::command("missing 'path'"));
            }
            require_project_loaded(ctx)?;
            if ctx.scene_edit.previewing() {
                return Err(Error::command("exit the asset preview first"));
            }
            let bake = ctx
                .assets
                .import_model(&params.path, saffron_assets::ImportOptions::default())
                .map_err(|e| Error::command(e.to_string()))?;
            let name = ctx
                .assets
                .catalog
                .find(bake.model_id)
                .map(|entry| entry.name.clone())
                .unwrap_or_default();
            Ok(ImportModelResult {
                id: WireUuid(bake.model_id.value()),
                name,
                r#type: "model".to_owned(),
            })
        },
    );

    reg.register::<InstantiateModelParams, EntityRef>(
        "instantiate-model",
        "instantiate-model {asset} [name]",
        |ctx, params| {
            require_project_loaded(ctx)?;
            let id = resolve_asset(ctx, &params.asset)?;
            let entry_type = ctx.assets.catalog.find(id).map(|e| e.asset_type);
            let entry_name = ctx
                .assets
                .catalog
                .find(id)
                .map(|e| e.name.clone())
                .unwrap_or_default();
            if entry_type != Some(AssetType::Model) {
                return Err(Error::command(format!(
                    "asset {} is not a model",
                    id.value()
                )));
            }
            let name = match &params.name {
                Some(name) if !name.is_empty() => name.clone(),
                _ => entry_name,
            };
            let root = ctx
                .assets
                .instantiate_model(ctx.scene_edit.active_scene(), id, &name)
                .map_err(|e| Error::command(e.to_string()))?;
            ctx.scene_edit.scene_version += 1;
            ctx.scene_edit.set_selection(root);
            let scene = ctx.scene_edit.active_scene();
            Ok(entity_ref_dto(scene, root))
        },
    );

    reg.register::<PathParams, ImportTextureResult>(
        "import-texture",
        "import-texture {path}",
        |ctx, params| {
            if params.path.is_empty() {
                return Err(Error::command("missing 'path'"));
            }
            require_project_loaded(ctx)?;
            let assets = &mut *ctx.assets;
            let path = params.path.clone();
            let mut result = None;
            ctx.renderer.with_gpu_uploader(&mut |gpu| {
                result = Some(assets.import_texture(gpu, &path));
            });
            let id = result
                .ok_or_else(|| Error::command("upload seam unavailable"))?
                .map_err(|e| Error::command(e.to_string()))?;
            Ok(ImportTextureResult {
                texture: WireUuid(id.value()),
            })
        },
    );

    reg.register::<EmptyParams, AssetList>(
        "list-assets",
        "list the project asset catalog",
        |ctx, _params| Ok(asset_list_dto(&ctx.assets.catalog)),
    );

    reg.register::<EmptyParams, ScanAssetsResult>("scan-assets", "scan-assets", |ctx, _params| {
        require_project_loaded(ctx)?;
        ctx.renderer.wait_gpu_idle();
        ctx.assets.clear_asset_caches();
        let delta = ctx
            .assets
            .scan_assets()
            .map_err(|e| Error::command(e.to_string()))?;
        ctx.assets.write_catalog_cache();
        Ok(ScanAssetsResult {
            added: i32::try_from(delta.added.len()).unwrap_or(i32::MAX),
            removed: i32::try_from(delta.removed.len()).unwrap_or(i32::MAX),
        })
    });

    reg.register::<ExtractSubAssetParams, AssetRef>(
        "extract-subasset",
        "extract-subasset {asset} {subAsset} [dest]",
        |ctx, params| {
            require_project_loaded(ctx)?;
            let model_id = resolve_asset(ctx, &params.asset)?;
            let dest = params.dest.clone().unwrap_or_default();
            let extracted =
                extract_sub_asset(ctx.assets, model_id, Uuid(params.sub_asset.0), &dest)
                    .map_err(|e| Error::command(e.to_string()))?;
            let name = ctx
                .assets
                .catalog
                .find(extracted)
                .map(|e| e.name.clone())
                .unwrap_or_default();
            Ok(AssetRef {
                id: WireUuid(extracted.value()),
                name,
                folder: None,
            })
        },
    );

    reg.register::<ClearExtractionParams, AssetRef>(
        "clear-extraction",
        "clear-extraction {asset} {subAsset}",
        |ctx, params| {
            require_project_loaded(ctx)?;
            let model_id = resolve_asset(ctx, &params.asset)?;
            let sub_id = Uuid(params.sub_asset.0);
            clear_extraction(ctx.assets, model_id, sub_id)
                .map_err(|e| Error::command(e.to_string()))?;
            let name = ctx
                .assets
                .catalog
                .find(sub_id)
                .map(|e| e.name.clone())
                .unwrap_or_default();
            Ok(AssetRef {
                id: WireUuid(sub_id.value()),
                name,
                folder: None,
            })
        },
    );

    reg.register::<ReimportModelParams, ReimportModelResult>(
        "reimport-model",
        "reimport-model {asset}",
        |ctx, params| {
            require_project_loaded(ctx)?;
            let id = resolve_asset(ctx, &params.asset)?;
            ctx.renderer.wait_gpu_idle();
            let delta =
                reimport_model(ctx.assets, id).map_err(|e| Error::command(e.to_string()))?;
            ctx.scene_edit.scene_version += 1;
            Ok(ReimportModelResult {
                updated: i32::try_from(delta.updated.len()).unwrap_or(i32::MAX),
                added: i32::try_from(delta.added.len()).unwrap_or(i32::MAX),
                removed_from_source: i32::try_from(delta.removed_from_source.len())
                    .unwrap_or(i32::MAX),
                skipped: delta.skipped,
            })
        },
    );

    reg.register::<ModelInfoParams, ModelInfoResult>(
        "model-info",
        "model-info {asset}",
        |ctx, params| {
            require_project_loaded(ctx)?;
            let id = resolve_asset(ctx, &params.asset)?;
            if ctx.assets.catalog.find(id).map(|e| e.asset_type) != Some(AssetType::Model) {
                return Err(Error::command(format!(
                    "asset {} is not a model",
                    id.value()
                )));
            }
            let path = ctx
                .assets
                .catalog
                .find(id)
                .map(|e| e.path.clone())
                .unwrap_or_default();
            let model = ctx
                .assets
                .load_model_asset(id)
                .ok_or_else(|| Error::command(format!("model {} is not loadable", id.value())))?;
            let meta = model.meta.clone();
            let total_bytes = std::fs::metadata(ctx.assets.root.join(&path))
                .map(|m| m.len())
                .unwrap_or(0);
            let mut material_count = 0;
            let mut sub_assets = Vec::new();
            for sub in &meta.sub_assets {
                if sub.asset_type == AssetType::Material {
                    material_count += 1;
                }
                let sub_row = AssetEntry {
                    id: sub.sub_id,
                    asset_type: sub.asset_type,
                    container: id,
                    ..AssetEntry::default()
                };
                let bytes = asset_bytes(ctx.assets, &sub_row);
                sub_assets.push(ModelSubAssetDto {
                    id: WireUuid(sub.sub_id.value()),
                    name: sub.name.clone(),
                    r#type: asset_type_name(sub.asset_type).to_owned(),
                    bytes,
                });
            }
            Ok(ModelInfoResult {
                id: WireUuid(id.value()),
                name: meta.name.clone(),
                source_path: meta.import.source_path.clone(),
                source_hash: meta.import.source_hash.clone(),
                material_count,
                has_skin: !meta.skin.is_null(),
                node_count: i32::try_from(meta.nodes.as_array().map_or(0, Vec::len))
                    .unwrap_or(i32::MAX),
                total_bytes,
                sub_assets,
            })
        },
    );

    reg.register::<AssetReferencesParams, AssetReferencesResult>(
        "asset-references",
        "asset-references {asset}",
        |ctx, params| {
            require_project_loaded(ctx)?;
            let id = resolve_asset(ctx, &params.asset)?;
            let assets = &mut *ctx.assets;
            let scene = ctx.scene_edit.active_scene();
            let graph = build_dependency_graph(scene, assets);
            Ok(AssetReferencesResult {
                referenced_by: graph
                    .referenced_by(id)
                    .iter()
                    .map(|u| u.value().to_string())
                    .collect(),
                references: graph
                    .references_of(id)
                    .iter()
                    .map(|u| u.value().to_string())
                    .collect(),
                footprint: graph.footprint(id),
            })
        },
    );

    reg.register::<GetAssetModelParams, AssetModelResult>(
        "get-asset-model",
        "get-asset-model {asset} — a model's capabilities + bone tree + clips, from its .smodel container",
        |ctx, params| {
            require_project_loaded(ctx)?;
            let id = resolve_asset(ctx, &params.asset)?;
            let entry = ctx
                .assets
                .catalog
                .find(id)
                .ok_or_else(|| Error::command(format!("no asset '{}'", id.value())))?;
            let container_id = if entry.asset_type == AssetType::Model {
                id
            } else {
                entry.container
            };
            if container_id.value() == 0 {
                return Err(Error::command(format!(
                    "asset {} is not part of a model container",
                    id.value()
                )));
            }
            let model = ctx.assets.load_model_asset(container_id).ok_or_else(|| {
                Error::command(format!("model {} is not loadable", container_id.value()))
            })?;
            let meta = model.meta.clone();
            let node_count = meta.nodes.as_array().map_or(0, Vec::len);
            let has_rig = !meta.skin.is_null();
            let bones = if has_rig { build_bone_tree(&meta) } else { Vec::new() };
            let clips = container_clips(&meta);
            let mesh_count = meta
                .sub_assets
                .iter()
                .filter(|s| s.asset_type == AssetType::Mesh)
                .count();
            let material_count = meta
                .sub_assets
                .iter()
                .filter(|s| s.asset_type == AssetType::Material)
                .count();
            Ok(AssetModelResult {
                mesh: WireUuid(container_id.value()),
                name: meta.name.clone(),
                capabilities: AssetCapabilitiesDto {
                    mesh_count: i32::try_from(mesh_count).unwrap_or(i32::MAX),
                    material_count: i32::try_from(material_count).unwrap_or(i32::MAX),
                    node_count: i32::try_from(node_count).unwrap_or(i32::MAX),
                    has_rig,
                    bone_count: i32::try_from(bones.len()).unwrap_or(i32::MAX),
                    clip_count: i32::try_from(clips.len()).unwrap_or(i32::MAX),
                },
                bones,
                clips,
            })
        },
    );

    reg.register::<GetAssetModelParams, AssetPreviewResultWrap>(
        "enter-asset-preview",
        "enter-asset-preview {asset} — open any model in an isolated preview scene",
        enter_asset_preview,
    );

    reg.register::<EmptyParams, PlayStateResult>(
        "exit-asset-preview",
        "exit-asset-preview — close the asset preview and restore the authored scene + camera",
        |ctx, _params| {
            if ctx.scene_edit.previewing() {
                ctx.renderer.set_active_view(ViewId::Scene);
            }
            leave_asset_preview(ctx.scene_edit);
            Ok(play_state_result(ctx))
        },
    );

    reg.register::<SetActiveViewParams, SetActiveViewResult>(
        "set-active-view",
        "set-active-view {view} — switch the rendered view (scene | assetPreview)",
        |ctx, params| {
            let view = ViewId::from_wire(&params.view).ok_or_else(|| {
                Error::command(format!(
                    "unknown view '{}' (expected 'scene' or 'assetPreview')",
                    params.view
                ))
            })?;
            if view == ViewId::AssetPreview
                && ctx.renderer.view_desired_size(ViewId::AssetPreview).0 == 0
            {
                let (w, h) = (
                    ctx.renderer.viewport_width(),
                    ctx.renderer.viewport_height(),
                );
                let _ = ctx
                    .renderer
                    .set_view_desired_size(ViewId::AssetPreview, w, h);
            }
            ctx.renderer.set_active_view(view);
            if view == ViewId::AssetPreview {
                activate_preview_view(ctx.scene_edit);
            } else {
                deactivate_preview_view(ctx.scene_edit);
            }
            Ok(SetActiveViewResult {
                view: view.wire().to_owned(),
            })
        },
    );

    reg.register::<CleanAssetsParams, CleanReport>(
        "clean-assets",
        "clean-assets [exclude...]",
        |ctx, params| {
            require_project_loaded(ctx)?;
            let exclude: Vec<Uuid> = params
                .exclude
                .unwrap_or_default()
                .iter()
                .map(|id| Uuid(id.parse::<u64>().unwrap_or(0)))
                .collect();
            let assets = &mut *ctx.assets;
            let scene = ctx.scene_edit.active_scene();
            let data = analyze_clean(scene, assets, &exclude);
            Ok(CleanReport {
                reclaimable_bytes: data.reclaimable_bytes,
                candidates: data
                    .candidates
                    .into_iter()
                    .map(|c| CleanCandidateDto {
                        id: WireUuid(c.id.value()),
                        path: c.path,
                        category: c.category.name().to_owned(),
                        bytes: c.bytes,
                        reason: c.reason,
                    })
                    .collect(),
            })
        },
    );

    reg.register::<DeleteUnusedParams, DeleteUnusedResult>(
        "delete-unused",
        "delete-unused {ids...} {confirm}",
        |ctx, params| {
            require_project_loaded(ctx)?;
            let ids: Vec<Uuid> = params
                .ids
                .iter()
                .map(|id| Uuid(id.parse::<u64>().unwrap_or(0)))
                .collect();
            ctx.renderer.wait_gpu_idle();
            ctx.assets.clear_asset_caches();
            let confirm = params.confirm.unwrap_or(false);
            let assets = &mut *ctx.assets;
            let scene = ctx.scene_edit.active_scene();
            let deleted = delete_unused(assets, scene, &ids, confirm)
                .map_err(|e| Error::command(e.to_string()))?;
            ctx.scene_edit.scene_version += 1;
            Ok(DeleteUnusedResult {
                deleted: deleted.deleted,
                reclaimed_bytes: deleted.reclaimed_bytes,
            })
        },
    );

    reg.register::<RenameAssetParams, AssetRef>(
        "rename-asset",
        "rename-asset {id|name, newName}",
        |ctx, params| {
            let selector = selector_string(&params.asset);
            if selector.is_empty() || params.name.is_empty() {
                return Err(Error::command("usage: rename-asset {id|name} {newName}"));
            }
            let by_id = selector.parse::<u64>().unwrap_or(0);
            for entry in &mut ctx.assets.catalog.entries {
                if entry.id.value() == by_id || entry.name == selector {
                    entry.name = params.name.clone();
                    return Ok(asset_ref(entry));
                }
            }
            Err(Error::command(format!("no asset '{selector}'")))
        },
    );

    reg.register::<CreateAssetFolderParams, AssetList>(
        "create-asset-folder",
        "create-asset-folder {folder}",
        |ctx, params| {
            if !valid_folder_path(&params.folder) {
                return Err(Error::command(
                    "folder must be a non-empty path without empty segments",
                ));
            }
            if !has_folder(&ctx.assets.catalog, &params.folder) {
                ctx.assets.catalog.folders.push(params.folder.clone());
                ctx.scene_edit.scene_version += 1;
            }
            Ok(asset_list_dto(&ctx.assets.catalog))
        },
    );

    reg.register::<RenameAssetFolderParams, AssetList>(
        "rename-asset-folder",
        "rename-asset-folder {folder, name}",
        |ctx, params| {
            if !valid_folder_path(&params.name) {
                return Err(Error::command(
                    "folder path must be non-empty and cannot contain empty segments",
                ));
            }
            if !has_folder(&ctx.assets.catalog, &params.folder) {
                return Err(Error::command(format!(
                    "no asset folder '{}'",
                    params.folder
                )));
            }
            if params.folder == params.name {
                return Ok(asset_list_dto(&ctx.assets.catalog));
            }
            if is_folder_descendant(&params.name, &params.folder) {
                return Err(Error::command("asset folder cannot be moved inside itself"));
            }
            if has_folder(&ctx.assets.catalog, &params.name) {
                return Err(Error::command(format!(
                    "asset folder '{}' already exists",
                    params.name
                )));
            }
            for folder in &mut ctx.assets.catalog.folders {
                if *folder == params.folder || is_folder_descendant(folder, &params.folder) {
                    *folder = replace_folder_prefix(folder, &params.folder, &params.name);
                }
            }
            for entry in &mut ctx.assets.catalog.entries {
                if entry.folder == params.folder
                    || is_folder_descendant(&entry.folder, &params.folder)
                {
                    entry.folder =
                        replace_folder_prefix(&entry.folder, &params.folder, &params.name);
                }
            }
            ctx.scene_edit.scene_version += 1;
            Ok(asset_list_dto(&ctx.assets.catalog))
        },
    );

    reg.register::<DeleteAssetFolderParams, AssetList>(
        "delete-asset-folder",
        "delete-asset-folder {folder}",
        |ctx, params| {
            let mut removed = false;
            let mut folders = Vec::with_capacity(ctx.assets.catalog.folders.len());
            for folder in &ctx.assets.catalog.folders {
                if *folder == params.folder || is_folder_descendant(folder, &params.folder) {
                    removed = true;
                } else {
                    folders.push(folder.clone());
                }
            }
            if !removed {
                return Err(Error::command(format!(
                    "no asset folder '{}'",
                    params.folder
                )));
            }
            ctx.assets.catalog.folders = folders;
            for entry in &mut ctx.assets.catalog.entries {
                if entry.folder == params.folder
                    || is_folder_descendant(&entry.folder, &params.folder)
                {
                    entry.folder.clear();
                }
            }
            ctx.scene_edit.scene_version += 1;
            Ok(asset_list_dto(&ctx.assets.catalog))
        },
    );

    reg.register::<MoveAssetParams, AssetRef>(
        "move-asset",
        "move-asset {asset, folder?}",
        |ctx, params| {
            let index = resolve_asset_index(ctx, &params.asset)?;
            let folder = params.folder.clone().unwrap_or_default();
            if !folder.is_empty() && !has_folder(&ctx.assets.catalog, &folder) {
                return Err(Error::command(format!("no asset folder '{folder}'")));
            }
            ctx.assets.catalog.entries[index].folder = folder;
            ctx.scene_edit.scene_version += 1;
            Ok(asset_ref(&ctx.assets.catalog.entries[index]))
        },
    );

    reg.register::<AssetUsagesParams, AssetUsagesResult>(
        "asset-usages",
        "asset-usages {asset}",
        |ctx, params| {
            let id = resolve_asset(ctx, &params.asset)?;
            let usages = collect_asset_usages(ctx.scene_edit.active_scene(), id);
            Ok(AssetUsagesResult { usages })
        },
    );

    reg.register::<AssetMetadataParams, AssetMetadataDto>(
        "probe-asset",
        "probe-asset {asset}",
        |ctx, params| {
            let id = resolve_asset(ctx, &params.asset)?;
            let entry = ctx
                .assets
                .catalog
                .find(id)
                .ok_or_else(|| Error::command(format!("no asset '{}'", id.value())))?
                .clone();
            let abs = ctx.assets.root.join(&entry.path);
            let metadata = std::fs::metadata(&abs);
            let size_bytes = metadata.as_ref().map(|m| m.len()).unwrap_or(0);
            let created_at = metadata
                .ok()
                .and_then(|m| m.modified().ok())
                .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                .map_or(0, |d| i64::try_from(d.as_secs()).unwrap_or(i64::MAX));
            let mut vertex_count = None;
            let mut triangle_count = None;
            if entry.asset_type == AssetType::Mesh
                && let Ok(counts) = ctx.assets.mesh_counts_for_asset(&entry)
            {
                vertex_count = Some(counts.vertex_count);
                triangle_count = Some(counts.index_count / 3);
            }
            Ok(AssetMetadataDto {
                id: WireUuid(entry.id.value()),
                name: entry.name.clone(),
                r#type: asset_type_dto(entry.asset_type),
                path: entry.path.clone(),
                folder: optional_folder(&entry.folder),
                size_bytes,
                vertex_count,
                triangle_count,
                created_at,
            })
        },
    );

    reg.register::<DeleteAssetParams, DeleteAssetResult>(
        "delete-asset",
        "delete-asset {asset}",
        |ctx, params| {
            if ctx.scene_edit.play_state != PlayState::Edit {
                return Err(Error::command("stop play first"));
            }
            if ctx.scene_edit.previewing() {
                return Err(Error::command("exit the asset preview first"));
            }
            let index = resolve_asset_index(ctx, &params.asset)?;
            let entry = ctx.assets.catalog.entries[index].clone();
            let cleared = clear_asset_usages(&mut ctx.scene_edit.scene, entry.id);
            ctx.assets.catalog.entries.remove(index);
            rebuild_asset_index(&mut ctx.assets.catalog);
            ctx.assets.mesh_by_uuid.remove(&entry.id.value());
            ctx.assets.texture_by_uuid.remove(&entry.id.value());
            let file_deleted = if entry.path.is_empty() {
                false
            } else {
                std::fs::remove_file(ctx.assets.root.join(&entry.path)).is_ok()
            };
            ctx.assets.remove_thumbnail_cache_for_asset(entry.id);
            ctx.scene_edit.scene_version += 1;
            Ok(DeleteAssetResult {
                id: WireUuid(entry.id.value()),
                name: entry.name.clone(),
                cleared,
                file_deleted,
            })
        },
    );

    reg.register::<AssignAssetParams, AssignAssetResult>(
        "assign-asset",
        "assign-asset {entity, slot:mesh|albedo|metallic-roughness, id|name}",
        |ctx, params| {
            if ctx.scene_edit.previewing() {
                return Err(Error::command("exit the asset preview first"));
            }
            let entity = resolve_entity(ctx, &params.entity)?;
            let selector = selector_string(&params.asset);
            let clearing = selector == "0"
                || selector.is_empty()
                || params.asset.as_u64() == Some(0)
                || params.asset.as_i64() == Some(0);
            let (assign_id, assign_name) = if clearing {
                (Uuid(0), String::new())
            } else {
                let id = resolve_asset(ctx, &params.asset)?;
                let name = ctx
                    .assets
                    .catalog
                    .find(id)
                    .map(|e| e.name.clone())
                    .unwrap_or_default();
                (id, name)
            };
            let scene = ctx.scene_edit.active_scene();
            match params.slot {
                AssetSlotDto::Mesh => {
                    if !scene.has_component::<Mesh>(entity) {
                        let _ = scene.add_component(entity, Mesh::default());
                    }
                    let _ = scene.with_component_mut::<Mesh, _>(entity, |m| m.mesh = assign_id);
                }
                AssetSlotDto::Albedo => {
                    ensure_material(scene, entity);
                    let _ = scene.with_component_mut::<Material, _>(entity, |m| {
                        m.albedo_texture = assign_id
                    });
                }
                AssetSlotDto::MetallicRoughness => {
                    ensure_material(scene, entity);
                    let _ = scene.with_component_mut::<Material, _>(entity, |m| {
                        m.metallic_roughness_texture = assign_id;
                    });
                }
                AssetSlotDto::Normal => {
                    ensure_material(scene, entity);
                    let _ = scene.with_component_mut::<Material, _>(entity, |m| {
                        m.normal_texture = assign_id
                    });
                }
                AssetSlotDto::Occlusion => {
                    ensure_material(scene, entity);
                    let _ = scene.with_component_mut::<Material, _>(entity, |m| {
                        m.occlusion_texture = assign_id;
                    });
                }
                AssetSlotDto::Emissive => {
                    ensure_material(scene, entity);
                    let _ = scene.with_component_mut::<Material, _>(entity, |m| {
                        m.emissive_texture = assign_id;
                    });
                }
                AssetSlotDto::Height => {
                    ensure_material(scene, entity);
                    let _ = scene.with_component_mut::<Material, _>(entity, |m| {
                        m.height_texture = assign_id
                    });
                }
            }
            ctx.scene_edit.scene_version += 1;
            Ok(AssignAssetResult {
                id: WireUuid(assign_id.value()),
                name: assign_name,
                slot: params.slot,
            })
        },
    );

    reg.register::<MaterialCreateParams, MaterialCreateResult>(
        "material-create",
        "material-create {name} [from-entity]",
        |ctx, params| {
            let asset = default_material_asset();
            let name = if params.name.is_empty() {
                "Material".to_owned()
            } else {
                params.name.clone()
            };
            let id = save_material_asset(ctx.assets, &asset, &name, "")
                .map_err(|e| Error::command(e.to_string()))?;
            ctx.scene_edit.scene_version += 1;
            Ok(MaterialCreateResult {
                id: WireUuid(id.value()),
                name,
            })
        },
    );

    reg.register::<MaterialAssignParams, MaterialAssignResult>(
        "material-assign",
        "material-assign {entity, material:id|name}",
        |ctx, params| {
            let entity = resolve_entity(ctx, &params.entity)?;
            let selector = selector_string(&params.material);
            let clearing =
                selector == "0" || selector.is_empty() || params.material.as_u64() == Some(0);
            let mat_id = if clearing {
                Uuid(0)
            } else {
                resolve_asset(ctx, &params.material)?
            };
            let scene = ctx.scene_edit.active_scene();
            if !scene.has_component::<MaterialAssetComponent>(entity) {
                let _ = scene.add_component(entity, MaterialAssetComponent::default());
            }
            let _ = scene
                .with_component_mut::<MaterialAssetComponent, _>(entity, |m| m.material = mat_id);
            ctx.scene_edit.scene_version += 1;
            Ok(MaterialAssignResult {
                material: WireUuid(mat_id.value()),
            })
        },
    );

    reg.register::<MaterialImportParams, MaterialImportResultDto>(
        "material-import",
        "material-import {path} [name]",
        |ctx, params| {
            let assets = &mut *ctx.assets;
            let path = params.path.clone();
            let name = params.name.clone();
            let mut result = None;
            ctx.renderer.with_gpu_uploader(&mut |gpu| {
                result = Some(import_material_folder(assets, gpu, &path, &name));
            });
            let imported = result
                .ok_or_else(|| Error::command("upload seam unavailable"))?
                .map_err(|e| Error::command(e.to_string()))?;
            ctx.scene_edit.scene_version += 1;
            Ok(MaterialImportResultDto {
                id: WireUuid(imported.material.value()),
                roles: imported.roles,
            })
        },
    );

    reg.register::<EmptyParams, MaterialListResult>(
        "material-list",
        "material-list",
        |ctx, _params| {
            let materials = ctx
                .assets
                .catalog
                .entries
                .iter()
                .filter(|e| e.asset_type == AssetType::Material)
                .map(|e| MaterialRefDto {
                    id: WireUuid(e.id.value()),
                    name: e.name.clone(),
                    folder: e.folder.clone(),
                })
                .collect();
            Ok(MaterialListResult { materials })
        },
    );

    reg.register::<MaterialGetParams, MaterialGetResult>(
        "material-get",
        "material-get {id|name}",
        |ctx, params| {
            let id = resolve_asset(ctx, &params.material)?;
            let m =
                load_material_asset(ctx.assets, id).map_err(|e| Error::command(e.to_string()))?;
            let graph = load_material_asset_raw(ctx.assets, id)
                .ok()
                .filter(|raw| raw.graph.is_object())
                .map_or_else(|| json!({}), |raw| raw.graph);
            Ok(MaterialGetResult {
                id: WireUuid(id.value()),
                blend: m.blend.clone(),
                unlit: m.unlit,
                base_color: vec4(m.base_color),
                metallic: m.metallic,
                roughness: m.roughness,
                emissive: vec3(m.emissive),
                emissive_strength: m.emissive_strength,
                albedo_texture: WireUuid(m.albedo_texture.value()),
                orm_texture: WireUuid(m.orm_texture.value()),
                normal_texture: WireUuid(m.normal_texture.value()),
                emissive_texture: WireUuid(m.emissive_texture.value()),
                height_texture: WireUuid(m.height_texture.value()),
                graph,
            })
        },
    );

    reg.register::<MaterialUpdateParams, MaterialUpdateResult>(
        "material-update",
        "material-update {id} [baseColor metallic roughness emissive emissiveStrength]",
        |ctx, params| {
            let id = resolve_asset(ctx, &params.material)?;
            let mut m =
                load_material_asset(ctx.assets, id).map_err(|e| Error::command(e.to_string()))?;
            if let Some(base) = params.base_color {
                m.base_color = from_vec4(base);
            }
            if let Some(metallic) = params.metallic {
                m.metallic = metallic;
            }
            if let Some(roughness) = params.roughness {
                m.roughness = roughness;
            }
            if let Some(emissive) = params.emissive {
                m.emissive = from_vec3(emissive);
            }
            if let Some(strength) = params.emissive_strength {
                m.emissive_strength = strength;
            }
            if let Some(normal_strength) = params.normal_strength {
                m.normal_strength = normal_strength;
            }
            if let Some(tex) = params.albedo_texture {
                m.albedo_texture = Uuid(tex.0);
            }
            if let Some(tex) = params.orm_texture {
                m.orm_texture = Uuid(tex.0);
            }
            if let Some(tex) = params.normal_texture {
                m.normal_texture = Uuid(tex.0);
            }
            if let Some(tex) = params.emissive_texture {
                m.emissive_texture = Uuid(tex.0);
            }
            if let Some(tex) = params.height_texture {
                m.height_texture = Uuid(tex.0);
            }
            update_material_asset(ctx.assets, id, &m).map_err(|e| Error::command(e.to_string()))?;
            ctx.scene_edit.scene_version += 1;
            Ok(MaterialUpdateResult {
                id: WireUuid(id.value()),
            })
        },
    );

    reg.register::<PreviewRenderParams, PreviewRenderResult>(
        "preview-render",
        "preview-render {material} [size]",
        |ctx, params| {
            let id = resolve_asset(ctx, &params.material)?;
            let loaded =
                load_material_asset(ctx.assets, id).map_err(|e| Error::command(e.to_string()))?;
            let size = params.size.unwrap_or(256);
            // A non-foldable graph (procedural nodes) renders through a codegen'd preview
            // shader; a foldable graph already folded into `loaded`, so the default studio
            // preview shows it. Resolved here (the closure below borrows `assets`), matching
            // the C++ `preview-render`'s `codegenSpv`.
            let codegen_spv = preview_codegen_spv(ctx.assets, id);
            let assets = &mut *ctx.assets;
            let mut png = None;
            ctx.renderer.with_thumbnail_gpu(&mut |gpu| {
                let sm = assets.resolve_material_asset(gpu, &loaded);
                png = Some((|| {
                    let tex = gpu.render_material_preview(&sm, size, codegen_spv.as_deref())?;
                    gpu.encode_texture_thumbnail_png(&tex, size, PngTransfer::Clamp)
                })());
            });
            let png = png
                .ok_or_else(|| Error::command("thumbnail seam unavailable"))?
                .map_err(|e| Error::command(e.to_string()))?;
            Ok(PreviewRenderResult {
                png: base64_encode(&png.bytes),
            })
        },
    );

    reg.register::<MaterialSetGraphParams, MaterialSetGraphResult>(
        "material-set-graph",
        "material-set-graph {material, graph}",
        |ctx, params| {
            let id = resolve_asset(ctx, &params.material)?;
            let mut m =
                load_material_asset(ctx.assets, id).map_err(|e| Error::command(e.to_string()))?;
            m.graph = params.graph.clone();
            let mut folded = m.clone();
            let foldable = lower_graph_to_params(&m.graph, &mut folded);
            if foldable {
                m = folded;
            }
            update_material_asset(ctx.assets, id, &m).map_err(|e| Error::command(e.to_string()))?;
            if !foldable {
                let _ = ctx.assets.compile_material_mesh_shader(&m.graph, id);
            }
            ctx.scene_edit.scene_version += 1;
            Ok(MaterialSetGraphResult {
                id: WireUuid(id.value()),
                foldable,
            })
        },
    );

    reg.register::<MaterialCreateInstanceParams, MaterialCreateResult>(
        "material-create-instance",
        "material-create-instance {parent} [name]",
        |ctx, params| {
            let parent = resolve_asset(ctx, &params.parent)?;
            let mut child = default_material_asset();
            child.parent = parent;
            let name = if params.name.is_empty() {
                "Instance".to_owned()
            } else {
                params.name.clone()
            };
            let id = save_material_asset(ctx.assets, &child, &name, "")
                .map_err(|e| Error::command(e.to_string()))?;
            ctx.scene_edit.scene_version += 1;
            Ok(MaterialCreateResult {
                id: WireUuid(id.value()),
                name,
            })
        },
    );

    reg.register::<MaterialSetOverrideParams, MaterialSetOverrideResult>(
        "material-set-override",
        "material-set-override {material, field, value}",
        |ctx, params| {
            let id = resolve_asset(ctx, &params.material)?;
            let mut m = load_material_asset_raw(ctx.assets, id)
                .map_err(|e| Error::command(e.to_string()))?;
            if !m.overrides.is_object() {
                m.overrides = json!({});
            }
            if let Some(map) = m.overrides.as_object_mut() {
                map.insert(params.field.clone(), params.value.clone());
            }
            update_material_asset(ctx.assets, id, &m).map_err(|e| Error::command(e.to_string()))?;
            ctx.scene_edit.scene_version += 1;
            Ok(MaterialSetOverrideResult {
                id: WireUuid(id.value()),
            })
        },
    );

    reg.register::<MaterialCompileParams, MaterialCompileResult>(
        "material-compile-graph",
        "material-compile-graph {material}",
        |ctx, params| {
            let id = resolve_asset(ctx, &params.material)?;
            let raw = load_material_asset_raw(ctx.assets, id)
                .map_err(|e| Error::command(e.to_string()))?;
            if !raw.graph.is_object() || raw.graph.as_object().is_none_or(|g| g.is_empty()) {
                return Err(Error::command("material has no node graph to compile"));
            }
            ctx.assets
                .compile_material_graph(&raw.graph, id)
                .map_err(|e| Error::command(e.to_string()))?;
            Ok(MaterialCompileResult {
                id: WireUuid(id.value()),
                ok: true,
            })
        },
    );

    reg.register::<EmptyParams, MaterialCookResult>(
        "material-cook",
        "material-cook",
        |ctx, _params| {
            let material_ids: Vec<Uuid> = ctx
                .assets
                .catalog
                .entries
                .iter()
                .filter(|e| e.asset_type == AssetType::Material)
                .map(|e| e.id)
                .collect();
            let mut compiled = 0u32;
            let mut failed = 0u32;
            for id in material_ids {
                let Ok(raw) = load_material_asset_raw(ctx.assets, id) else {
                    continue;
                };
                if !raw.graph.is_object() || raw.graph.as_object().is_none_or(|g| g.is_empty()) {
                    continue;
                }
                let mut probe = raw.clone();
                if lower_graph_to_params(&raw.graph, &mut probe) {
                    continue;
                }
                if ctx
                    .assets
                    .compile_material_mesh_shader(&raw.graph, id)
                    .is_ok()
                {
                    compiled += 1;
                } else {
                    failed += 1;
                }
            }
            Ok(MaterialCookResult { compiled, failed })
        },
    );

    reg.register::<PathParams, PathResult>("save-scene", "save-scene {path}", |ctx, params| {
        if params.path.is_empty() {
            return Err(Error::command("missing 'path'"));
        }
        let editor = &mut *ctx.scene_edit;
        editor
            .scene
            .write_scene(&editor.registry, &params.path)
            .map_err(|e| Error::command(e.to_string()))?;
        editor.scene_path = params.path.clone();
        Ok(PathResult { path: params.path })
    });

    reg.register::<PathParams, PathResult>("load-scene", "load-scene {path}", |ctx, params| {
        if ctx.scene_edit.play_state != PlayState::Edit {
            return Err(Error::command("stop play first"));
        }
        if params.path.is_empty() {
            return Err(Error::command("missing 'path'"));
        }
        {
            let editor = &mut *ctx.scene_edit;
            editor
                .scene
                .read_scene(&editor.registry, &params.path)
                .map_err(|e| Error::command(e.to_string()))?;
        }
        ctx.scene_edit.scene_path = params.path.clone();
        ctx.scene_edit.scene_version += 1;
        ctx.scene_edit.set_selection(Entity::NULL);
        Ok(PathResult { path: params.path })
    });

    reg.register::<OptionalPathParams, ProjectInfoDto>(
        "save-project",
        "save-project {path} — assets catalog + scene in one file",
        |ctx, params| {
            let mut path = params.path.clone().unwrap_or_default();
            let mut project = current_project_info(ctx);
            if path.is_empty() {
                path = project.path.clone();
            }
            if path.is_empty() {
                return Err(Error::command("no active project path"));
            }
            if !project.loaded {
                let fs_path = Path::new(&path);
                project.loaded = true;
                project.path = path.clone();
                let parent = fs_path.parent();
                project.root = match parent {
                    Some(p) if !p.as_os_str().is_empty() => p.to_string_lossy().into_owned(),
                    _ => ".".to_owned(),
                };
                let dir_name = parent
                    .and_then(|p| p.file_name())
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_default();
                project.name = if valid_project_name(&dir_name) {
                    dir_name
                } else {
                    "project".to_owned()
                };
                project.display_name = default_display_name(&project.name);
            }
            let sidecar = saffron_assets::ProjectSidecar {
                editor_camera: ctx.scene_edit.camera.to_json(),
                debug_overlays: saffron_sceneedit::debug_overlays_to_json(
                    &ctx.scene_edit.debug_overlays,
                ),
            };
            let host = RendererProjectHost {
                renderer: ctx.renderer,
            };
            ctx.assets
                .save_project(
                    &host,
                    &ctx.scene_edit.registry,
                    &mut ctx.scene_edit.scene,
                    &project,
                    &path,
                    &sidecar,
                )
                .map_err(|e| Error::command(e.to_string()))?;
            project.path = path;
            apply_project_info(ctx, &project);
            Ok(project_dto(&project))
        },
    );

    reg.register::<OptionalPathParams, ProjectInfoDto>(
        "load-project",
        "load-project {path} — assets catalog + scene",
        |ctx, params| {
            if ctx.scene_edit.play_state != PlayState::Edit {
                return Err(Error::command("stop play first"));
            }
            if ctx.scene_edit.previewing() {
                return Err(Error::command("exit the asset preview first"));
            }
            let path = params
                .path
                .clone()
                .unwrap_or_else(|| "project.json".to_owned());
            let project = load_project_into(ctx, &path)?;
            Ok(project_dto(&project))
        },
    );

    reg.register::<EmptyParams, ProjectInfoDto>(
        "reload-project",
        "reload-project — close and re-open the active project",
        |ctx, _params| {
            if ctx.scene_edit.play_state != PlayState::Edit {
                return Err(Error::command("stop play first"));
            }
            if ctx.scene_edit.previewing() {
                return Err(Error::command("exit the asset preview first"));
            }
            require_project_loaded(ctx)?;
            let path = ctx.scene_edit.project_path.clone();
            let project = load_project_into(ctx, &path)?;
            Ok(project_dto(&project))
        },
    );

    reg.register::<ScreenshotParams, ScreenshotResult>(
        "screenshot",
        "screenshot {target:viewport|window, path}",
        |ctx, params| {
            let target = params.target.unwrap_or(ScreenshotTargetDto::Viewport);
            if params.path.is_empty() {
                return Err(Error::command("missing 'path'"));
            }
            match target {
                ScreenshotTargetDto::Viewport => {
                    ctx.renderer
                        .capture_viewport(Path::new(&params.path))
                        .map_err(Error::Command)?;
                    Ok(ScreenshotResult {
                        target,
                        path: params.path,
                        pending: false,
                    })
                }
                ScreenshotTargetDto::Window => {
                    ctx.renderer
                        .request_window_capture(Path::new(&params.path))
                        .map_err(Error::Command)?;
                    Ok(ScreenshotResult {
                        target,
                        path: params.path,
                        pending: true,
                    })
                }
            }
        },
    );

    reg.register::<ThumbnailParams, ThumbnailResult>(
        "get-thumbnail",
        "get-thumbnail {asset:id|name, size=128} — base64 PNG preview",
        |ctx, params| thumbnail_result(ctx, &params, 128),
    );

    reg.register::<ThumbnailParams, ThumbnailResult>(
        "view-asset",
        "view-asset {asset:id|name, size=512} — larger base64 PNG preview",
        |ctx, params| thumbnail_result(ctx, &params, 512),
    );

    reg.register::<ThumbnailCacheParams, ThumbnailCacheResult>(
        "thumbnail-cache",
        "thumbnail-cache {action: stats|clear} — inspect or empty the disk cache",
        |ctx, params| {
            if params.action == "clear" {
                let removed = ctx.assets.clear_thumbnail_cache_dir();
                return Ok(ThumbnailCacheResult {
                    entries: i32::try_from(removed.entries).unwrap_or(i32::MAX),
                    bytes: i64::try_from(removed.bytes).unwrap_or(i64::MAX),
                });
            }
            if params.action == "stats" || params.action.is_empty() {
                let stats = ctx.assets.thumbnail_cache_stats();
                return Ok(ThumbnailCacheResult {
                    entries: i32::try_from(stats.entries).unwrap_or(i32::MAX),
                    bytes: i64::try_from(stats.bytes).unwrap_or(i64::MAX),
                });
            }
            Err(Error::command(format!(
                "unknown action '{}' (stats|clear)",
                params.action
            )))
        },
    );

    reg.register::<EmptyParams, QuitResult>("quit", "close the running app", |ctx, _params| {
        ctx.window.request_close();
        Ok(QuitResult { quitting: true })
    });
}

/// The codegen `_preview.spv` path for `preview-render`, or `None` for the default studio
/// preview. A material whose raw graph is a non-empty object that *does not* fold into the
/// flat params (a procedural node) gets a freshly compiled preview shader; a foldable or
/// graph-less material renders through the cached default pipeline. The C++ `preview-render`
/// `codegenSpv` block. A compile failure degrades to `None` (the default preview still
/// renders the folded params), never an error.
fn preview_codegen_spv(assets: &AssetServer, id: Uuid) -> Option<PathBuf> {
    let raw = load_material_asset_raw(assets, id).ok()?;
    let non_empty_graph = raw.graph.as_object().is_some_and(|obj| !obj.is_empty());
    if !non_empty_graph {
        return None;
    }
    let mut probe = raw.clone();
    if lower_graph_to_params(&raw.graph, &mut probe) {
        return None;
    }
    assets.compile_material_preview_shader(&raw.graph, id).ok()
}

/// The `screenshot {target:window}` is the only command reaching `window`; the rest reach
/// assets/sceneEdit/renderer. Ensures the entity carries a [`Material`] (the C++
/// `addComponent<MaterialComponent>` guard) before an `assign-asset` texture slot write.
fn ensure_material(scene: &mut Scene, entity: Entity) {
    if !scene.has_component::<Material>(entity) {
        let _ = scene.add_component(entity, Material::default());
    }
}

/// The shared `load-project` / `reload-project` body: idle + reload the catalog + scene +
/// sidecar, then reset the editor state (the C++ `loadProject` command bodies).
fn load_project_into(ctx: &mut EngineContext<'_>, path: &str) -> Result<ProjectInfo> {
    let defs = ctx.renderer.sa_lua_defs();
    let mut project = ProjectInfo::default();
    let mut host = RendererProjectHost {
        renderer: ctx.renderer,
    };
    let sidecar = ctx
        .assets
        .load_project(
            &mut host,
            &ctx.scene_edit.registry,
            &mut ctx.scene_edit.scene,
            &mut project,
            path,
            &defs,
        )
        .map_err(|e| Error::command(e.to_string()))?;
    apply_project_info(ctx, &project);
    ctx.scene_edit.camera.from_json(&sidecar.editor_camera);
    saffron_sceneedit::debug_overlays_from_json(
        &mut ctx.scene_edit.debug_overlays,
        &sidecar.debug_overlays,
    );
    ctx.scene_edit.scene_version += 1;
    ctx.scene_edit.script_input = saffron_scene::ScriptInputState::default();
    ctx.scene_edit.set_selection(Entity::NULL);
    Ok(project)
}

/// The `enter-asset-preview` body: build an isolated preview scene, commit it, furnish it
/// (floor / key light / procedural sky / framed fly-cam), and route the renderer + active
/// view to it (the C++ `enter-asset-preview` handler).
fn enter_asset_preview(
    ctx: &mut EngineContext<'_>,
    params: GetAssetModelParams,
) -> Result<AssetPreviewResultWrap> {
    require_project_loaded(ctx)?;
    if ctx.scene_edit.play_state != PlayState::Edit {
        return Err(Error::command("stop play first"));
    }
    let id = resolve_asset(ctx, &params.asset)?;
    let entry = ctx
        .assets
        .catalog
        .find(id)
        .ok_or_else(|| Error::command(format!("no asset '{}'", id.value())))?;
    let entry_type = entry.asset_type;
    let container_id = if entry_type == AssetType::Model {
        id
    } else {
        entry.container
    };
    if container_id.value() == 0 {
        return Err(Error::command(format!(
            "asset {} is not part of a model container",
            id.value()
        )));
    }
    let model = ctx
        .assets
        .load_model_asset(container_id)
        .ok_or_else(|| Error::command(format!("model {} is not loadable", container_id.value())))?;
    let meta = model.meta.clone();

    // Build the preview scene locally so a failed swap stays on the prior model; commit only
    // once the model spawned a renderable mesh. Instantiation references mesh ids by uuid —
    // the GPU upload happens lazily at render — so no upload seam is needed here.
    let mut preview = Scene::new();
    preview.catalog = ctx.scene_edit.scene.catalog.clone();
    let root = ctx
        .assets
        .instantiate_model(&mut preview, container_id, &meta.name)
        .map_err(|e| Error::command(e.to_string()))?;

    let animatable = preview.animatable_descendant(root);
    let rigged = preview.has_component::<SkinnedMesh>(animatable);
    if !rigged && !preview.has_component::<Mesh>(animatable) {
        return Err(Error::command(format!(
            "model '{}' has no renderable mesh — re-import the asset",
            meta.name
        )));
    }

    // Open-from-clip: that clip becomes the active clip; the model opens paused at rest.
    if entry_type == AssetType::Animation && preview.has_component::<AnimationPlayer>(animatable) {
        let _ = preview.with_component_mut::<AnimationPlayer, _>(animatable, |player| {
            player.clip = id;
            player.time = 0.0;
            player.playing = false;
            player.preview_in_edit = false;
        });
    }

    let root_uuid = preview
        .component::<IdComponent>(root)
        .map(|c| c.id.value())
        .unwrap_or(0);
    let mut bone_by_node = Vec::new();
    let mut bones = Vec::new();
    if rigged {
        let bone_uuids = preview
            .with_component::<SkinnedMesh, _>(animatable, |skin| skin.bones.clone())
            .unwrap_or_default();
        let joint_nodes: Vec<i32> = meta
            .skin
            .get("joints")
            .and_then(Value::as_array)
            .map(|joints| {
                joints
                    .iter()
                    .filter_map(|j| j.as_i64().map(|v| v as i32))
                    .collect()
            })
            .unwrap_or_default();
        let node_count = meta.nodes.as_array().map_or(0, Vec::len);
        bone_by_node = vec![Uuid(0); node_count];
        let joint_count = joint_nodes.len().min(bone_uuids.len());
        for k in 0..joint_count {
            let node_idx = joint_nodes[k];
            let uuid = bone_uuids[k];
            if node_idx >= 0 && (node_idx as usize) < node_count && uuid.value() != 0 {
                bone_by_node[node_idx as usize] = uuid;
                bones.push(saffron_protocol::BoneEntityDto {
                    index: node_idx,
                    entity: WireUuid(uuid.value()),
                });
            }
        }
    }

    // Commit: stash camera + selection + overlay only on a fresh enter (a swap keeps the
    // authored stash). A fresh enter makes the preview the active view.
    if !ctx.scene_edit.previewing() {
        ctx.scene_edit.saved_camera = ctx.scene_edit.camera;
        ctx.scene_edit.saved_selection = ctx.scene_edit.selected;
        ctx.scene_edit.saved_overlay = ctx.scene_edit.skeleton_overlay;
        ctx.scene_edit.preview_active_view = true;
        let (w, h) = (
            ctx.renderer.viewport_width(),
            ctx.renderer.viewport_height(),
        );
        let _ = ctx
            .renderer
            .set_view_desired_size(ViewId::AssetPreview, w, h);
        ctx.renderer.set_active_view(ViewId::AssetPreview);
    }
    ctx.scene_edit.preview_scene = Some(preview);
    ctx.scene_edit.preview_asset = container_id;
    ctx.scene_edit.preview_root_entity = root;
    ctx.scene_edit.preview_bone_by_node = bone_by_node;
    ctx.scene_edit.preview_floor_entity = Entity::NULL;
    ctx.scene_edit.skeleton_overlay.show = true;
    ctx.scene_edit.skeleton_overlay.highlight_joint = -1;
    let framing = furnish_preview_scene(ctx, root);
    ctx.scene_edit.set_selection(root);
    ctx.scene_edit.scene_version += 1;
    ctx.scene_edit.animation_version += 1;
    Ok(AssetPreviewResultWrap(
        saffron_protocol::AssetPreviewResult {
            root_entity: WireUuid(root_uuid),
            bones,
            target: vec3(framing.target),
            distance: framing.distance,
        },
    ))
}

/// A newtype around [`AssetPreviewResult`](saffron_protocol::AssetPreviewResult) so the
/// `enter-asset-preview` handler can use a free fn (the closure form does not infer the
/// generic). It serializes transparently to the wire DTO.
#[derive(serde::Serialize)]
#[serde(transparent)]
pub struct AssetPreviewResultWrap(saffron_protocol::AssetPreviewResult);

/// The preview framing pivot + orbit distance (the C++ `PreviewFraming`).
struct PreviewFraming {
    target: saffron_geometry::glam::Vec3,
    distance: f32,
}

/// The previewed model's world-space bounding sphere from its mesh's rest-pose AABB (the
/// C++ `computePreviewBounds`).
pub(crate) struct PreviewBounds {
    center: saffron_geometry::glam::Vec3,
    radius: f32,
    min_y: f32,
}

/// Make the preview look like a preview: floor / key light / procedural sky / framed
/// fly-cam; returns the orbit pivot + distance (the C++ `furnishPreviewScene`). Operates on
/// the committed preview scene.
fn furnish_preview_scene(ctx: &mut EngineContext<'_>, root: Entity) -> PreviewFraming {
    use saffron_geometry::glam::Vec3 as GVec3;

    let bounds = compute_preview_bounds(ctx, root);
    if ctx.scene_edit.preview_show_floor {
        let floor = spawn_preview_floor(ctx, &bounds);
        ctx.scene_edit.preview_floor_entity = floor;
    }

    let preview = ctx
        .scene_edit
        .preview_scene
        .as_mut()
        .expect("preview scene present");
    let light = preview.create_entity("PreviewLight");
    let _ = preview.add_component(
        light,
        DirectionalLight {
            direction: GVec3::new(-0.4, -1.0, -0.5).normalize(),
            color: GVec3::ONE,
            intensity: 3.0,
            ambient: 0.25,
        },
    );
    preview.environment.sky_mode = SkyMode::Procedural;
    preview.environment.use_sky_for_ambient = true;
    preview.environment.ambient_intensity = 0.3;

    ctx.scene_edit.camera = frame_preview_camera(ctx.scene_edit.camera, &bounds);
    let fovy = ctx.scene_edit.camera.fov.to_radians();
    PreviewFraming {
        target: bounds.center,
        distance: bounds.radius / (fovy * 0.5).tan() * 1.3,
    }
}

/// The previewed model's world-space bounding sphere (the C++ `computePreviewBounds`).
pub(crate) fn compute_preview_bounds(ctx: &mut EngineContext<'_>, root: Entity) -> PreviewBounds {
    use saffron_geometry::glam::Vec3 as GVec3;

    let mut out = PreviewBounds {
        center: GVec3::ZERO,
        radius: 1.0,
        min_y: 0.0,
    };
    let preview = ctx
        .scene_edit
        .preview_scene
        .as_mut()
        .expect("preview scene present");
    if !preview.valid(root) {
        return out;
    }
    let mesh_entity = preview.animatable_descendant(root);
    let mesh_id = preview
        .with_component::<SkinnedMesh, _>(mesh_entity, |s| s.mesh)
        .ok()
        .or_else(|| {
            preview
                .with_component::<Mesh, _>(mesh_entity, |m| m.mesh)
                .ok()
        })
        .unwrap_or(Uuid(0));
    preview.update_world_transforms();
    let world = preview.world_matrix(mesh_entity);
    let mesh_world_translation = preview.world_translation(mesh_entity);

    let assets = &mut *ctx.assets;
    let mut gpu_bounds = None;
    if mesh_id.value() != 0 {
        ctx.renderer.with_gpu_uploader(&mut |gpu| {
            if let Some(mesh) = assets.load_mesh_asset(gpu, mesh_id) {
                gpu_bounds = Some((mesh.bounds_min, mesh.bounds_max));
            }
        });
    }
    let Some((bmin, bmax)) = gpu_bounds else {
        out.center = mesh_world_translation;
        out.min_y = out.center.y - 1.0;
        return out;
    };
    let (lo, hi) = world_aabb_from_corners(world, bmin, bmax);
    out.center = (lo + hi) * 0.5;
    out.radius = (hi - lo).length() * 0.5;
    out.min_y = lo.y;
    if out.radius <= 0.0001 {
        out.radius = 1.0;
    }
    out
}

/// The world-space AABB of a local box transformed by `world`, expanded over its eight
/// corners (the C++ `worldAabbFromCorners`).
fn world_aabb_from_corners(
    world: saffron_geometry::glam::Mat4,
    lo: saffron_geometry::glam::Vec3,
    hi: saffron_geometry::glam::Vec3,
) -> (saffron_geometry::glam::Vec3, saffron_geometry::glam::Vec3) {
    use saffron_geometry::glam::{Vec3, Vec4};

    let mut out_lo = Vec3::splat(f32::MAX);
    let mut out_hi = Vec3::splat(f32::MIN);
    for i in 0..8 {
        let corner = Vec3::new(
            if i & 1 == 0 { lo.x } else { hi.x },
            if i & 2 == 0 { lo.y } else { hi.y },
            if i & 4 == 0 { lo.z } else { hi.z },
        );
        let world_corner = (world * Vec4::new(corner.x, corner.y, corner.z, 1.0)).truncate();
        out_lo = out_lo.min(world_corner);
        out_hi = out_hi.max(world_corner);
    }
    (out_lo, out_hi)
}

/// A thin floor slab centered under the model's feet (the C++ `spawnPreviewFloor`).
pub(crate) fn spawn_preview_floor(ctx: &mut EngineContext<'_>, bounds: &PreviewBounds) -> Entity {
    use saffron_geometry::glam::{Vec3 as GVec3, Vec4 as GVec4};

    let assets = &mut *ctx.assets;
    let mut ensured = false;
    ctx.renderer.with_gpu_uploader(&mut |gpu| {
        ensured = assets.ensure_preview_floor_mesh(gpu);
    });
    if !ensured {
        return Entity::NULL;
    }
    let preview = ctx
        .scene_edit
        .preview_scene
        .as_mut()
        .expect("preview scene present");
    let floor = preview.create_entity("PreviewFloor");
    let _ = preview.add_component(
        floor,
        Mesh {
            mesh: saffron_assets::PREVIEW_FLOOR_MESH_ID,
        },
    );
    let _ = preview.add_component(
        floor,
        Material {
            base_color: GVec4::new(0.32, 0.33, 0.35, 1.0),
            roughness: 0.92,
            metallic: 0.0,
            ..Material::default()
        },
    );
    let span = (bounds.radius * 8.0).max(0.5);
    let thickness = (bounds.radius * 0.08).max(0.02);
    let _ = preview.with_component_mut::<Transform, _>(floor, |t| {
        t.translation = GVec3::new(
            bounds.center.x,
            bounds.min_y - thickness * 0.5,
            bounds.center.z,
        );
        t.scale = GVec3::new(span, thickness, span);
    });
    floor
}

/// Aim a fly-cam at the model: a 3/4 view fit to its bounding sphere (the C++
/// `framePreviewCamera`). Starts from the current camera so the user's fov/near/far survive.
fn frame_preview_camera(mut cam: SceneEditCamera, bounds: &PreviewBounds) -> SceneEditCamera {
    use saffron_geometry::glam::Vec3 as GVec3;

    let fovy = cam.fov.to_radians();
    let distance = bounds.radius / (fovy * 0.5).tan() * 1.3;
    let eye = bounds.center + GVec3::new(1.0, 0.7, 1.0).normalize() * distance;
    let forward = (bounds.center - eye).normalize();
    cam.position = eye;
    cam.pitch = forward.y.clamp(-1.0, 1.0).asin().to_degrees();
    cam.yaw = forward.x.atan2(-forward.z).to_degrees();
    cam.far_plane = cam.far_plane.max(distance + bounds.radius * 4.0);
    cam.near_plane = (distance * 0.01).clamp(1e-4, 0.1);
    cam
}

/// Builds the [`PlayStateResult`] from the editor state (the C++ `playStateResultDto`).
fn play_state_result(ctx: &EngineContext<'_>) -> PlayStateResult {
    let editor = &ctx.scene_edit;
    PlayStateResult {
        state: editor.play_state.name().to_owned(),
        play_version: i32::try_from(editor.play_version).unwrap_or(i32::MAX),
        scene_version: i32::try_from(editor.scene_version).unwrap_or(i32::MAX),
        has_primary_camera: editor.had_primary_camera,
        animation_version: i32::try_from(editor.animation_version).unwrap_or(i32::MAX),
        preview_asset: WireUuid(editor.preview_asset.value()),
    }
}

/// Converts a glam `Vec3` into the wire `Vec3`.
fn vec3(v: saffron_geometry::glam::Vec3) -> Vec3 {
    Vec3 {
        x: v.x,
        y: v.y,
        z: v.z,
    }
}

/// Converts a glam `Vec4` into the wire `Vec4`.
fn vec4(v: saffron_geometry::glam::Vec4) -> Vec4 {
    Vec4 {
        x: v.x,
        y: v.y,
        z: v.z,
        w: v.w,
    }
}

/// Converts a wire `Vec3` into a glam vector.
fn from_vec3(v: Vec3) -> saffron_geometry::glam::Vec3 {
    saffron_geometry::glam::Vec3::new(v.x, v.y, v.z)
}

/// Converts a wire `Vec4` into a glam vector.
fn from_vec4(v: Vec4) -> saffron_geometry::glam::Vec4 {
    saffron_geometry::glam::Vec4::new(v.x, v.y, v.z, v.w)
}

#[cfg(test)]
mod tests {
    use saffron_scene::{AssetEntry, AssetType, Material, Mesh};
    use serde_json::json;

    use super::preview_codegen_spv;

    use crate::registry::{CommandRegistry, EngineContext, register_builtin_commands};
    use crate::selector::entity_uuid;
    use crate::test_support::{StubRenderer, with_stub};

    fn registry() -> CommandRegistry {
        let mut reg = CommandRegistry::new();
        register_builtin_commands(&mut reg);
        reg
    }

    /// Roots the test's asset server at a unique scratch dir so the material file-writing
    /// commands never collide across parallel tests.
    fn scratch_root(ctx: &mut EngineContext<'_>, tag: &str) {
        let dir = std::env::temp_dir().join(format!(
            "saffron-control-asset-{tag}-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        ctx.assets.set_asset_root(dir.join("assets"));
    }

    /// Seeds a mesh catalog row, returning its decimal-string id.
    fn seed_mesh(ctx: &mut EngineContext<'_>, name: &str) -> u64 {
        let id = saffron_core::Uuid::new();
        ctx.assets.catalog.put(AssetEntry {
            id,
            name: name.to_owned(),
            asset_type: AssetType::Mesh,
            path: format!("models/{}.smesh", id.value()),
            ..AssetEntry::default()
        });
        id.value()
    }

    /// `list-assets` and `scan-assets` round-trip on an empty (just-loaded) project.
    #[test]
    fn list_and_scan_on_empty_project() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            scratch_root(ctx, "empty");
            ctx.scene_edit.project_loaded = true;

            let list = reg.dispatch(ctx, &json!({ "cmd": "list-assets" }));
            assert_eq!(list["ok"], json!(true));
            assert_eq!(list["result"]["assets"], json!([]));
            assert_eq!(list["result"]["folders"], json!([]));

            let scan = reg.dispatch(ctx, &json!({ "cmd": "scan-assets" }));
            assert_eq!(scan["ok"], json!(true));
            assert_eq!(scan["result"]["added"], json!(0));
            assert_eq!(scan["result"]["removed"], json!(0));
        });
    }

    /// `scan-assets` (and the other project-gated commands) refuse without a loaded project.
    #[test]
    fn scan_assets_requires_a_project() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            let scan = reg.dispatch(ctx, &json!({ "cmd": "scan-assets" }));
            assert_eq!(scan["ok"], json!(false));
            assert_eq!(scan["error"], json!("no project loaded"));
        });
    }

    /// `assign-asset` resolves an `AssetSelector` by id and by name, assigns the mesh slot,
    /// and returns a decimal-string id matching the catalog row.
    #[test]
    fn assign_asset_resolves_id_and_name() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            let mesh_id = seed_mesh(ctx, "cube");
            let entity = ctx.scene_edit.active_scene().create_entity("box");
            let entity_uuid = entity_uuid(ctx.scene_edit.active_scene(), entity).to_string();

            // By numeric id.
            let by_id = reg.dispatch(
                ctx,
                &json!({ "cmd": "assign-asset", "params": { "entity": entity_uuid, "slot": "mesh", "asset": mesh_id.to_string() } }),
            );
            assert_eq!(by_id["ok"], json!(true));
            assert_eq!(by_id["result"]["id"], json!(mesh_id.to_string()));
            assert_eq!(by_id["result"]["slot"], json!("mesh"));
            assert_eq!(by_id["result"]["name"], json!("cube"));
            assert_eq!(
                ctx.scene_edit
                    .active_scene()
                    .component::<Mesh>(entity)
                    .unwrap()
                    .mesh
                    .value(),
                mesh_id
            );

            // By name selector.
            let by_name = reg.dispatch(
                ctx,
                &json!({ "cmd": "assign-asset", "params": { "entity": entity_uuid, "slot": "mesh", "asset": "cube" } }),
            );
            assert_eq!(by_name["result"]["id"], json!(mesh_id.to_string()));

            // The null sentinel (id 0) clears the slot rather than resolving an asset.
            let clear = reg.dispatch(
                ctx,
                &json!({ "cmd": "assign-asset", "params": { "entity": entity_uuid, "slot": "mesh", "asset": "0" } }),
            );
            assert_eq!(clear["result"]["id"], json!("0"));
            assert_eq!(
                ctx.scene_edit
                    .active_scene()
                    .component::<Mesh>(entity)
                    .unwrap()
                    .mesh
                    .value(),
                0
            );
        });
    }

    /// `assign-asset` on a texture slot attaches a `Material` and writes the texture id.
    #[test]
    fn assign_asset_albedo_attaches_material() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            let tex_id = {
                let id = saffron_core::Uuid::new();
                ctx.assets.catalog.put(AssetEntry {
                    id,
                    name: "albedo".to_owned(),
                    asset_type: AssetType::Texture,
                    ..AssetEntry::default()
                });
                id.value()
            };
            let entity = ctx.scene_edit.active_scene().create_entity("box");
            let entity_uuid = entity_uuid(ctx.scene_edit.active_scene(), entity).to_string();
            let reply = reg.dispatch(
                ctx,
                &json!({ "cmd": "assign-asset", "params": { "entity": entity_uuid, "slot": "albedo", "asset": tex_id.to_string() } }),
            );
            assert_eq!(reply["ok"], json!(true));
            assert_eq!(reply["result"]["slot"], json!("albedo"));
            assert_eq!(
                ctx.scene_edit
                    .active_scene()
                    .component::<Material>(entity)
                    .unwrap()
                    .albedo_texture
                    .value(),
                tex_id
            );
        });
    }

    /// `set-active-view` maps `scene` / `assetPreview` and errors on an unknown view (the
    /// C++ `viewIdFromWire` `Err` message).
    #[test]
    fn set_active_view_maps_and_errors() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            let scene = reg.dispatch(
                ctx,
                &json!({ "cmd": "set-active-view", "params": { "view": "scene" } }),
            );
            assert_eq!(scene["ok"], json!(true));
            assert_eq!(scene["result"]["view"], json!("scene"));

            let preview = reg.dispatch(
                ctx,
                &json!({ "cmd": "set-active-view", "params": { "view": "assetPreview" } }),
            );
            assert_eq!(preview["result"]["view"], json!("assetPreview"));

            let bad = reg.dispatch(
                ctx,
                &json!({ "cmd": "set-active-view", "params": { "view": "nope" } }),
            );
            assert_eq!(bad["ok"], json!(false));
            assert_eq!(
                bad["error"],
                json!("unknown view 'nope' (expected 'scene' or 'assetPreview')")
            );
        });
    }

    /// `rename-asset` renames the catalog row and returns the new `{id, name}`.
    #[test]
    fn rename_asset_round_trips() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            let id = seed_mesh(ctx, "old-name");
            let reply = reg.dispatch(
                ctx,
                &json!({ "cmd": "rename-asset", "params": { "asset": id.to_string(), "name": "new-name" } }),
            );
            assert_eq!(reply["ok"], json!(true));
            assert_eq!(reply["result"]["id"], json!(id.to_string()));
            assert_eq!(reply["result"]["name"], json!("new-name"));
            assert_eq!(
                ctx.assets
                    .catalog
                    .find(saffron_core::Uuid(id))
                    .unwrap()
                    .name,
                "new-name"
            );
        });
    }

    /// `create-asset-folder` adds a folder and `list-assets` reflects it; an invalid path
    /// errors.
    #[test]
    fn create_asset_folder_and_list() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            let made = reg.dispatch(
                ctx,
                &json!({ "cmd": "create-asset-folder", "params": { "folder": "props/crates" } }),
            );
            assert_eq!(made["ok"], json!(true));
            assert_eq!(made["result"]["folders"], json!(["props/crates"]));

            let bad = reg.dispatch(
                ctx,
                &json!({ "cmd": "create-asset-folder", "params": { "folder": "/leading" } }),
            );
            assert_eq!(bad["ok"], json!(false));
        });
    }

    /// `material-create` then `material-get` round-trips the `.smat`; `material-set-graph`
    /// stores an opaque graph that `material-get` reads back verbatim.
    #[test]
    fn material_set_graph_keeps_graph_opaque() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            scratch_root(ctx, "material-graph");
            let create = reg.dispatch(
                ctx,
                &json!({ "cmd": "material-create", "params": { "name": "Mat" } }),
            );
            assert_eq!(create["ok"], json!(true));
            let id = create["result"]["id"].as_str().unwrap().to_owned();

            // A graph object with a codegen-only shape (no fold) is stored verbatim.
            let graph = json!({ "nodes": [{ "id": 1, "type": "noise" }], "edges": [] });
            let set = reg.dispatch(
                ctx,
                &json!({ "cmd": "material-set-graph", "params": { "material": id, "graph": graph } }),
            );
            assert_eq!(set["ok"], json!(true), "set-graph: {set:?}");
            assert_eq!(set["result"]["id"], json!(id));

            let get = reg.dispatch(
                ctx,
                &json!({ "cmd": "material-get", "params": { "material": id } }),
            );
            assert_eq!(get["ok"], json!(true), "get: {get:?}");
            assert_eq!(get["result"]["graph"], graph, "graph round-trips opaque");
        });
    }

    /// `preview_codegen_spv` decides the preview pipeline: a graph-less material and a
    /// foldable graph both use the default studio preview (`None`); a non-foldable
    /// (procedural) graph compiles the `_preview.spv` when `slangc` is present and the path
    /// exists on disk (else degrades to `None` — never an error).
    #[test]
    fn preview_codegen_spv_picks_the_pipeline_per_graph() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            scratch_root(ctx, "preview-codegen");

            // A freshly created material has no graph → the default preview.
            let create = reg.dispatch(
                ctx,
                &json!({ "cmd": "material-create", "params": { "name": "Mat" } }),
            );
            let id_str = create["result"]["id"].as_str().unwrap().to_owned();
            let id = saffron_core::Uuid(id_str.parse::<u64>().unwrap());
            assert!(
                preview_codegen_spv(ctx.assets, id).is_none(),
                "a graph-less material uses the default preview"
            );

            // A foldable graph (a single constant into baseColor) folds into params → still
            // the default preview, no codegen.
            let foldable = json!({
                "nodes": [
                    { "id": "c1", "type": "constant", "props": { "value": [0.5, 0.25, 1.0, 1.0] } },
                    { "id": "out", "type": "materialOutput" }
                ],
                "edges": [ { "from": ["c1", "out"], "to": ["out", "baseColor"] } ]
            });
            let set = reg.dispatch(
                ctx,
                &json!({ "cmd": "material-set-graph", "params": { "material": id_str, "graph": foldable } }),
            );
            assert_eq!(set["result"]["foldable"], json!(true), "set: {set:?}");
            assert!(
                preview_codegen_spv(ctx.assets, id).is_none(),
                "a foldable graph uses the default preview"
            );

            // A non-foldable graph (a procedural multiply against a texture slot) needs the
            // codegen preview shader.
            let procedural = json!({
                "nodes": [
                    { "id": "c1", "type": "constant", "props": { "value": [0.5, 0.25, 1.0, 1.0] } },
                    { "id": "tx", "type": "textureSlot", "props": { "slot": "normal" } },
                    { "id": "mul", "type": "multiply" },
                    { "id": "out", "type": "materialOutput" }
                ],
                "edges": [
                    { "from": ["c1", "out"], "to": ["mul", "a"] },
                    { "from": ["tx", "out"], "to": ["mul", "b"] },
                    { "from": ["mul", "out"], "to": ["out", "baseColor"] }
                ]
            });
            let set = reg.dispatch(
                ctx,
                &json!({ "cmd": "material-set-graph", "params": { "material": id_str, "graph": procedural } }),
            );
            assert_eq!(set["result"]["foldable"], json!(false), "set: {set:?}");
            // The codegen path runs; with slangc present it yields an on-disk `_preview.spv`,
            // else it degrades to None. Whatever it returns, a Some path must exist.
            if let Some(spv) = preview_codegen_spv(ctx.assets, id) {
                assert!(
                    spv.exists(),
                    "the compiled _preview.spv is on disk: {spv:?}"
                );
                assert!(
                    spv.to_string_lossy().ends_with("_preview.spv"),
                    "the codegen artifact is the preview variant: {spv:?}"
                );
            } else {
                eprintln!("slangc not runnable: codegen preview degraded to the default");
            }
        });
    }

    /// `thumbnail-cache stats` reports a clean cache; an unknown action errors.
    #[test]
    fn thumbnail_cache_stats_and_unknown_action() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            scratch_root(ctx, "thumb-cache");
            let stats = reg.dispatch(
                ctx,
                &json!({ "cmd": "thumbnail-cache", "params": { "action": "stats" } }),
            );
            assert_eq!(stats["ok"], json!(true));
            assert_eq!(stats["result"]["entries"], json!(0));

            let bad = reg.dispatch(
                ctx,
                &json!({ "cmd": "thumbnail-cache", "params": { "action": "nope" } }),
            );
            assert_eq!(bad["ok"], json!(false));
            assert_eq!(bad["error"], json!("unknown action 'nope' (stats|clear)"));
        });
    }

    /// `get-project` reports the editor's project identity; it is loaded after a field set.
    #[test]
    fn get_project_reports_identity() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            ctx.scene_edit.project_loaded = true;
            ctx.scene_edit.project_name = "demo".to_owned();
            ctx.scene_edit.project_display_name = "Demo".to_owned();
            let reply = reg.dispatch(ctx, &json!({ "cmd": "get-project" }));
            assert_eq!(reply["ok"], json!(true));
            assert_eq!(reply["result"]["loaded"], json!(true));
            assert_eq!(reply["result"]["name"], json!("demo"));
            assert_eq!(reply["result"]["displayName"], json!("Demo"));
        });
    }

    /// The asset domain registers in the frozen manifest order (`get-project` … `quit`),
    /// contiguously at the tail of the registry — the order `help` + the contract test
    /// iterate.
    #[test]
    fn asset_commands_register_in_manifest_order() {
        const FROZEN: &[&str] = &[
            "get-project",
            "new-project",
            "create-script",
            "open-project",
            "import-model",
            "instantiate-model",
            "import-texture",
            "list-assets",
            "scan-assets",
            "extract-subasset",
            "clear-extraction",
            "reimport-model",
            "model-info",
            "asset-references",
            "get-asset-model",
            "enter-asset-preview",
            "exit-asset-preview",
            "set-active-view",
            "clean-assets",
            "delete-unused",
            "rename-asset",
            "create-asset-folder",
            "rename-asset-folder",
            "delete-asset-folder",
            "move-asset",
            "asset-usages",
            "probe-asset",
            "delete-asset",
            "assign-asset",
            "material-create",
            "material-assign",
            "material-import",
            "material-list",
            "material-get",
            "material-update",
            "preview-render",
            "material-set-graph",
            "material-create-instance",
            "material-set-override",
            "material-compile-graph",
            "material-cook",
            "save-scene",
            "load-scene",
            "save-project",
            "load-project",
            "reload-project",
            "screenshot",
            "get-thumbnail",
            "view-asset",
            "thumbnail-cache",
            "quit",
        ];
        let reg = registry();
        let names: Vec<&str> = reg.rows().iter().map(|c| c.name).collect();
        let start = names
            .iter()
            .position(|&n| n == "get-project")
            .expect("get-project is registered");
        assert_eq!(
            &names[start..start + FROZEN.len()],
            FROZEN,
            "the asset domain registers contiguously in the frozen manifest order"
        );
        // `quit` is the last command in the registry.
        assert_eq!(names.last(), Some(&"quit"));
    }

    /// `asset-usages` reports a mesh slot that references the queried asset, with the entity
    /// id as a decimal string.
    #[test]
    fn asset_usages_reports_mesh_slot() {
        let reg = registry();
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, |ctx| {
            let mesh_id = seed_mesh(ctx, "cube");
            let entity = ctx.scene_edit.active_scene().create_entity("box");
            let _ = ctx.scene_edit.active_scene().add_component(
                entity,
                Mesh {
                    mesh: saffron_core::Uuid(mesh_id),
                },
            );
            let entity_uuid = entity_uuid(ctx.scene_edit.active_scene(), entity).to_string();

            let reply = reg.dispatch(
                ctx,
                &json!({ "cmd": "asset-usages", "params": { "asset": mesh_id.to_string() } }),
            );
            assert_eq!(reply["ok"], json!(true));
            let usages = reply["result"]["usages"].as_array().unwrap();
            assert_eq!(usages.len(), 1);
            assert_eq!(usages[0]["slot"], json!("mesh"));
            assert_eq!(usages[0]["entity"], json!(entity_uuid));
        });
    }
}
