module;

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

module Saffron.Control;

import Saffron.Core;
import Saffron.Json;
import Saffron.Window;
import Saffron.Rendering;
import Saffron.Geometry;
import Saffron.Scene;
import Saffron.SceneEdit;
import Saffron.Assets;

namespace se
{
    namespace
    {
        auto currentProjectInfo(EngineContext& ctx) -> ProjectInfo
        {
            return ProjectInfo{ ctx.sceneEdit.projectLoaded, ctx.sceneEdit.projectRoot, ctx.sceneEdit.projectPath,
                                ctx.sceneEdit.projectName, ctx.sceneEdit.projectDisplayName };
        }

        void applyProjectInfo(EngineContext& ctx, const ProjectInfo& project)
        {
            ctx.sceneEdit.projectLoaded = project.loaded;
            ctx.sceneEdit.projectRoot = project.root;
            ctx.sceneEdit.projectPath = project.path;
            ctx.sceneEdit.projectName = project.name;
            ctx.sceneEdit.projectDisplayName = project.displayName;
            ctx.sceneEdit.scenePath = project.path;
        }

        auto projectDto(const ProjectInfo& project) -> ProjectInfoDto
        {
            return ProjectInfoDto{ project.loaded, project.root, project.path, project.name, project.displayName };
        }

        auto requireProjectLoaded(EngineContext& ctx) -> Result<void>
        {
            if (!ctx.sceneEdit.projectLoaded)
            {
                return Err(std::string{ "no project loaded" });
            }
            return {};
        }

        auto assetTypeDto(AssetType type) -> AssetTypeDto
        {
            if (type == AssetType::Texture)
            {
                return AssetTypeDto::Texture;
            }
            if (type == AssetType::Other)
            {
                return AssetTypeDto::Other;
            }
            if (type == AssetType::Animation)
            {
                return AssetTypeDto::Animation;
            }
            if (type == AssetType::Material)
            {
                return AssetTypeDto::Material;
            }
            if (type == AssetType::Model)
            {
                return AssetTypeDto::Model;
            }
            return AssetTypeDto::Mesh;
        }

        auto assetSlotName(AssetSlotDto slot) -> const char*
        {
            switch (slot)
            {
            case AssetSlotDto::Albedo:
                return "albedo";
            case AssetSlotDto::MetallicRoughness:
                return "metallic-roughness";
            case AssetSlotDto::Normal:
                return "normal";
            case AssetSlotDto::Occlusion:
                return "occlusion";
            case AssetSlotDto::Emissive:
                return "emissive";
            case AssetSlotDto::Height:
                return "height";
            case AssetSlotDto::Mesh:
                return "mesh";
            }
            return "mesh";
        }

        auto screenshotTargetName(ScreenshotTargetDto target) -> const char*
        {
            return target == ScreenshotTargetDto::Window ? "window" : "viewport";
        }

        auto resolveAsset(EngineContext& ctx, const AssetSelector& asset) -> Result<const AssetEntry*>
        {
            const json& sel = asset.value;
            const std::string selector = sel.is_string() ? sel.get<std::string>() : std::string{};
            u64 byId = 0;
            if (sel.is_number_unsigned())
            {
                byId = sel.get<u64>();
            }
            else if (sel.is_number_integer())
            {
                const i64 v = sel.get<i64>();
                if (v >= 0)
                {
                    byId = static_cast<u64>(v);
                }
            }
            else
            {
                byId = std::strtoull(selector.c_str(), nullptr, 10);
            }
            for (const AssetEntry& entry : ctx.assets.catalog.entries)
            {
                if (entry.id.value == byId || entry.name == selector)
                {
                    return &entry;
                }
            }
            return Err(std::format("no asset '{}'", selector));
        }

        auto resolveAssetIndex(EngineContext& ctx, const AssetSelector& asset) -> Result<std::size_t>
        {
            const json& sel = asset.value;
            const std::string selector = sel.is_string() ? sel.get<std::string>() : std::string{};
            u64 byId = 0;
            if (sel.is_number_unsigned())
            {
                byId = sel.get<u64>();
            }
            else if (sel.is_number_integer())
            {
                const i64 v = sel.get<i64>();
                if (v >= 0)
                {
                    byId = static_cast<u64>(v);
                }
            }
            else
            {
                byId = std::strtoull(selector.c_str(), nullptr, 10);
            }
            for (std::size_t i = 0; i < ctx.assets.catalog.entries.size(); i += 1)
            {
                const AssetEntry& entry = ctx.assets.catalog.entries[i];
                if (entry.id.value == byId || entry.name == selector)
                {
                    return i;
                }
            }
            return Err(std::format("no asset '{}'", selector));
        }

        void rebuildAssetIndex(AssetCatalog& catalog)
        {
            catalog.byId.clear();
            for (std::size_t i = 0; i < catalog.entries.size(); i += 1)
            {
                catalog.byId[catalog.entries[i].id.value] = i;
            }
        }

        auto assetDto(const AssetEntry& entry) -> AssetEntryDto
        {
            return AssetEntryDto{
                WireUuid{ entry.id.value },
                entry.name,
                assetTypeDto(entry.type),
                entry.path,
                entry.folder.empty() ? std::optional<std::string>{} : std::optional<std::string>{ entry.folder },
                entry.container.value == 0 ? std::optional<WireUuid>{}
                                           : std::optional<WireUuid>{ WireUuid{ entry.container.value } },
                entry.type == AssetType::Animation ? std::optional<f32>{ entry.duration } : std::optional<f32>{},
                entry.rigged ? std::optional<bool>{ true } : std::optional<bool>{}
            };
        }

        auto assetRef(const AssetEntry& entry) -> AssetRef
        {
            return AssetRef{ WireUuid{ entry.id.value }, entry.name,
                             entry.folder.empty() ? std::optional<std::string>{}
                                                  : std::optional<std::string>{ entry.folder } };
        }

        auto assetListDto(const AssetCatalog& catalog) -> AssetList
        {
            AssetList out;
            for (const AssetEntry& entry : catalog.entries)
            {
                out.assets.push_back(assetDto(entry));
            }
            out.folders = catalog.folders;
            return out;
        }

        auto validFolderPath(const std::string& folder) -> bool
        {
            if (folder.empty() || folder.front() == '/' || folder.back() == '/' ||
                folder.find('\\') != std::string::npos)
            {
                return false;
            }
            for (std::size_t i = 0; i < folder.size(); i = i + 1)
            {
                if (folder[i] == '/' && i + 1 < folder.size() && folder[i + 1] == '/')
                {
                    return false;
                }
            }
            return true;
        }

        auto hasFolder(const AssetCatalog& catalog, const std::string& folder) -> bool
        {
            for (const std::string& existing : catalog.folders)
            {
                if (existing == folder)
                {
                    return true;
                }
            }
            return false;
        }

        auto isFolderDescendant(const std::string& candidate, const std::string& folder) -> bool
        {
            return candidate.size() > folder.size() && candidate.starts_with(folder) && candidate[folder.size()] == '/';
        }

        auto replaceFolderPrefix(const std::string& value, const std::string& from, const std::string& to)
            -> std::string
        {
            if (value == from)
            {
                return to;
            }
            if (isFolderDescendant(value, from))
            {
                return to + value.substr(from.size());
            }
            return value;
        }

        auto entityName(Scene& scene, Entity entity) -> std::string
        {
            if (hasComponent<NameComponent>(scene, entity))
            {
                return getComponent<NameComponent>(scene, entity).name;
            }
            return std::string{};
        }

        auto entityId(Scene& scene, Entity entity) -> std::optional<WireUuid>
        {
            if (hasComponent<IdComponent>(scene, entity))
            {
                return WireUuid{ getComponent<IdComponent>(scene, entity).id.value };
            }
            return {};
        }

        auto collectAssetUsages(Scene& scene, Uuid asset) -> std::vector<AssetUsageDto>
        {
            std::vector<AssetUsageDto> usages;
            forEach<MeshComponent>(
                scene,
                [&](Entity entity, MeshComponent& mesh)
                {
                    if (mesh.mesh.value == asset.value)
                    {
                        usages.push_back(AssetUsageDto{ entityId(scene, entity), entityName(scene, entity), "mesh" });
                    }
                });
            forEach<MaterialComponent>(
                scene,
                [&](Entity entity, MaterialComponent& material)
                {
                    if (material.albedoTexture.value == asset.value)
                    {
                        usages.push_back(AssetUsageDto{ entityId(scene, entity), entityName(scene, entity), "albedo" });
                    }
                    if (material.metallicRoughnessTexture.value == asset.value)
                    {
                        usages.push_back(
                            AssetUsageDto{ entityId(scene, entity), entityName(scene, entity), "metallic-roughness" });
                    }
                });
            if (scene.environment.skyTexture.value == asset.value)
            {
                usages.push_back(AssetUsageDto{ {}, {}, "environment.skyTexture" });
            }
            return usages;
        }

        auto clearAssetUsages(Scene& scene, Uuid asset) -> std::vector<AssetUsageDto>
        {
            std::vector<AssetUsageDto> cleared;
            forEach<MeshComponent>(
                scene,
                [&](Entity entity, MeshComponent& mesh)
                {
                    if (mesh.mesh.value == asset.value)
                    {
                        cleared.push_back(AssetUsageDto{ entityId(scene, entity), entityName(scene, entity), "mesh" });
                        mesh.mesh = Uuid{};
                    }
                });
            forEach<MaterialComponent>(scene,
                                       [&](Entity entity, MaterialComponent& material)
                                       {
                                           if (material.albedoTexture.value == asset.value)
                                           {
                                               cleared.push_back(AssetUsageDto{ entityId(scene, entity),
                                                                                entityName(scene, entity), "albedo" });
                                               material.albedoTexture = Uuid{};
                                           }
                                           if (material.metallicRoughnessTexture.value == asset.value)
                                           {
                                               cleared.push_back(AssetUsageDto{ entityId(scene, entity),
                                                                                entityName(scene, entity),
                                                                                "metallic-roughness" });
                                               material.metallicRoughnessTexture = Uuid{};
                                           }
                                       });
            if (scene.environment.skyTexture.value == asset.value)
            {
                cleared.push_back(AssetUsageDto{ {}, {}, "environment.skyTexture" });
                scene.environment.skyTexture = Uuid{};
            }
            return cleared;
        }

        // Resolves {asset:id|name, size?} to a base64 PNG preview. The generation, disk cache, and
        // off-thread worker all live in Saffron.Assets (requestThumbnail): a cache hit returns the PNG,
        // a cold miss replies `pending` (the worker is generating) and the editor retries. Shared by
        // get-thumbnail (128) + view-asset (512).
        auto thumbnailResult(EngineContext& ctx, const ThumbnailParams& params, u32 defaultSize)
            -> Result<ThumbnailResult>
        {
            auto resolved = resolveAsset(ctx, params.asset);
            if (!resolved)
            {
                return Err(resolved.error());
            }
            const Uuid id = (*resolved)->id;
            const u32 size = static_cast<u32>(params.size.value_or(static_cast<i32>(defaultSize)));
            auto reply = requestThumbnail(ctx.assets, ctx.renderer, id, size);
            if (!reply)
            {
                return Err(reply.error());
            }
            if (reply->pending)
            {
                return ThumbnailResult{ WireUuid{ id.value }, "png", 0, 0, std::string{}, true };
            }
            return ThumbnailResult{ WireUuid{ id.value },           "png",
                                    static_cast<i32>(reply->width), static_cast<i32>(reply->height),
                                    base64Encode(reply->png),       false };
        }

        struct PreviewBounds
        {
            glm::vec3 center{ 0.0f };
            f32 radius = 1.0f;
            f32 minY = 0.0f;
        };

        // The previewed model's world-space bounding sphere, from its mesh's rest-pose AABB. The mesh
        // lives on the model's mesh entity — the root itself for a static model, or a descendant of the
        // container root for a skinned model.
        auto computePreviewBounds(Scene& scene, AssetServer& assets, Renderer& renderer, Entity root) -> PreviewBounds
        {
            PreviewBounds out;
            if (!valid(scene, root))
            {
                return out;
            }
            const Entity meshEntity = animatableDescendant(scene, root);
            Uuid meshId{ 0 };
            if (hasComponent<SkinnedMeshComponent>(scene, meshEntity))
            {
                meshId = getComponent<SkinnedMeshComponent>(scene, meshEntity).mesh;
            }
            else if (hasComponent<MeshComponent>(scene, meshEntity))
            {
                meshId = getComponent<MeshComponent>(scene, meshEntity).mesh;
            }
            updateWorldTransforms(scene);
            auto gpu = meshId.value != 0 ? loadMeshAsset(assets, renderer, meshId) : Ref<GpuMesh>{};
            if (!gpu)
            {
                out.center = worldTranslation(scene, meshEntity);
                out.minY = out.center.y - 1.0f;
                return out;
            }
            const glm::mat4 world = worldMatrix(scene, meshEntity);
            glm::vec3 lo{ 0.0f };
            glm::vec3 hi{ 0.0f };
            for (int c = 0; c < 8; c = c + 1)
            {
                const glm::vec3 corner{ (c & 1) ? gpu->boundsMax.x : gpu->boundsMin.x,
                                        (c & 2) ? gpu->boundsMax.y : gpu->boundsMin.y,
                                        (c & 4) ? gpu->boundsMax.z : gpu->boundsMin.z };
                const glm::vec3 w = glm::vec3(world * glm::vec4(corner, 1.0f));
                lo = c == 0 ? w : glm::min(lo, w);
                hi = c == 0 ? w : glm::max(hi, w);
            }
            out.center = (lo + hi) * 0.5f;
            out.radius = glm::length(hi - lo) * 0.5f;
            out.minY = lo.y;
            if (out.radius <= 0.0001f)
            {
                out.radius = 1.0f;
            }
            return out;
        }

        // A thin floor slab centered under the model's feet (reserved system mesh; no catalog row).
        auto spawnPreviewFloor(Scene& scene, AssetServer& assets, Renderer& renderer, const PreviewBounds& bounds)
            -> Entity
        {
            if (!ensurePreviewFloorMesh(assets, renderer))
            {
                return Entity{ entt::null };
            }
            Entity floor = createEntity(scene, "PreviewFloor");
            addComponent<MeshComponent>(scene, floor).mesh = PreviewFloorMeshId;
            MaterialComponent& mat = addComponent<MaterialComponent>(scene, floor);
            mat.baseColor = glm::vec4{ 0.32f, 0.33f, 0.35f, 1.0f };
            mat.roughness = 0.92f;
            mat.metallic = 0.0f;
            const f32 span = glm::max(0.5f, bounds.radius * 8.0f);  // cube is [-0.5,0.5]; span = full width
            const f32 thickness = glm::max(0.02f, bounds.radius * 0.08f);
            TransformComponent& transform = getComponent<TransformComponent>(scene, floor);
            transform.translation = glm::vec3{ bounds.center.x, bounds.minY - thickness * 0.5f, bounds.center.z };
            transform.scale = glm::vec3{ span, thickness, span };
            return floor;
        }

        // Aim a fly-cam at the model: a 3/4 view fit to its bounding sphere (the thumbnail framing). Starts
        // from the current camera so the user's fov/near/far/speed prefs survive, then reframes the pose.
        auto framePreviewCamera(SceneEditCamera cam, const PreviewBounds& bounds) -> SceneEditCamera
        {
            const f32 fovy = glm::radians(cam.fov);
            const f32 distance = bounds.radius / glm::tan(fovy * 0.5f) * 1.3f;
            const glm::vec3 eye = bounds.center + glm::normalize(glm::vec3{ 1.0f, 0.7f, 1.0f }) * distance;
            const glm::vec3 forward = glm::normalize(bounds.center - eye);
            cam.position = eye;
            cam.pitch = glm::degrees(std::asin(glm::clamp(forward.y, -1.0f, 1.0f)));
            cam.yaw = glm::degrees(std::atan2(forward.x, -forward.z));
            cam.farPlane = glm::max(cam.farPlane, distance + bounds.radius * 4.0f);
            // Scale the near plane to the framed distance so a small model can be dollied in close
            // without the stock 0.1 plane culling it; large models keep ~0.1 (the ceiling).
            cam.nearPlane = glm::clamp(distance * 0.01f, 1e-4f, 0.1f);
            return cam;
        }

        struct PreviewFraming
        {
            glm::vec3 target{ 0.0f };
            f32 distance = 1.0f;
        };

        // Make the preview look like a preview: a floor, a key light, a procedural sky, and the framed
        // fly-cam. Operates on the committed previewScene; writes edit.camera (after the caller has
        // stashed the authored one), stores the floor entity for the toggle, and returns the orbit
        // pivot + distance for the editor's orbit.
        auto furnishPreviewScene(SceneEditContext& edit, AssetServer& assets, Renderer& renderer, Entity root)
            -> PreviewFraming
        {
            Scene& scene = *edit.previewScene;
            const PreviewBounds bounds = computePreviewBounds(scene, assets, renderer, root);

            if (edit.previewShowFloor)
            {
                edit.previewFloorEntity = spawnPreviewFloor(scene, assets, renderer, bounds);
            }

            Entity light = createEntity(scene, "PreviewLight");
            DirectionalLightComponent& dl = addComponent<DirectionalLightComponent>(scene, light);
            dl.direction = glm::normalize(glm::vec3{ -0.4f, -1.0f, -0.5f });  // thumbnail.slang's neutral key
            dl.color = glm::vec3{ 1.0f };
            dl.intensity = 3.0f;
            dl.ambient = 0.25f;

            scene.environment.skyMode = SkyMode::Procedural;
            scene.environment.useSkyForAmbient = true;
            scene.environment.ambientIntensity = 0.3f;

            edit.camera = framePreviewCamera(edit.camera, bounds);
            const f32 fovy = glm::radians(edit.camera.fov);
            return PreviewFraming{ bounds.center, bounds.radius / glm::tan(fovy * 0.5f) * 1.3f };
        }

        // Drop the asset preview and restore the authored edit state: the fly-cam (stashed on enter, so
        // framing/orbit never dirty the saved editorCamera), the overlay prefs, the authored selection
        // (ctx.scene was never touched while previewing), and the version stamps the editor's poll keys
        // on. A no-op when not previewing.
        void leaveAssetPreview(SceneEditContext& ctx)
        {
            if (!ctx.previewScene)
            {
                return;
            }
            const bool wasActive = ctx.previewActiveView;
            ctx.previewScene.reset();
            ctx.previewAsset = Uuid{ 0 };
            ctx.previewRootEntity = Entity{ entt::null };
            ctx.previewFloorEntity = Entity{ entt::null };
            ctx.previewBoneByNode.clear();
            ctx.previewActiveView = false;
            // With the scene view active the authored fly-cam/overlay/selection are already live (the view
            // switch restored them), so only exiting from the active preview view restores the enter stash.
            if (wasActive)
            {
                ctx.camera = ctx.savedCamera;
                ctx.skeletonOverlay = ctx.savedOverlay;
                const Entity restore = ctx.savedSelection.handle != entt::null && valid(ctx.scene, ctx.savedSelection)
                                           ? ctx.savedSelection
                                           : Entity{ entt::null };
                ctx.savedSelection = Entity{ entt::null };
                setSelection(ctx, restore);
            }
            ctx.sceneVersion += 1;
            ctx.animationVersion += 1;
        }

        // Make the scene view active while a preview scene is alive: park the preview orbit camera and
        // restore the authored fly-cam/overlay/selection (so the scene view shows the authored scene).
        // activeScene routes back to the authored scene and previewing() clears, so it is editable again.
        // A no-op unless the preview is currently the active view.
        void deactivatePreviewView(SceneEditContext& ctx)
        {
            if (!ctx.previewScene || !ctx.previewActiveView)
            {
                return;
            }
            ctx.parkedPreviewCamera = ctx.camera;  // park the preview orbit
            ctx.camera = ctx.savedCamera;
            ctx.skeletonOverlay = ctx.savedOverlay;
            const Entity restore = ctx.savedSelection.handle != entt::null && valid(ctx.scene, ctx.savedSelection)
                                       ? ctx.savedSelection
                                       : Entity{ entt::null };
            setSelection(ctx, restore);
            ctx.previewActiveView = false;
            ctx.sceneVersion += 1;
            ctx.animationVersion += 1;
        }

        // Make the preview view active again: re-stash the authored view (the user may have moved the
        // fly-cam or changed selection while the scene view was active), restore the parked preview orbit
        // + overlay, and reselect the previewed root. No re-spawn — the preview scene persisted. A no-op
        // unless a preview scene is alive but not currently active.
        void activatePreviewView(SceneEditContext& ctx)
        {
            if (!ctx.previewScene || ctx.previewActiveView)
            {
                return;
            }
            ctx.savedCamera = ctx.camera;
            ctx.savedSelection = ctx.selected;
            ctx.savedOverlay = ctx.skeletonOverlay;
            ctx.camera = ctx.parkedPreviewCamera;
            ctx.skeletonOverlay.show = true;  // the preview forces the skeleton overlay on (mirrors enter)
            ctx.skeletonOverlay.highlightJoint = -1;
            ctx.previewActiveView = true;
            setSelection(ctx, ctx.previewRootEntity);
            ctx.sceneVersion += 1;
            ctx.animationVersion += 1;
        }
    }

    void registerAssetCommands(CommandRegistry& reg)
    {
        registerCommand<EmptyParams, ProjectInfoDto>(
            reg, "get-project", "get-project — active project metadata",
            [](EngineContext& ctx, const EmptyParams&) -> Result<ProjectInfoDto>
            { return projectDto(currentProjectInfo(ctx)); });

        registerCommand<NewProjectParams, ProjectInfoDto>(
            reg, "new-project", "new-project {name, displayName?, root?}",
            [](EngineContext& ctx, const NewProjectParams& params) -> Result<ProjectInfoDto>
            {
                if (ctx.sceneEdit.playState != PlayState::Edit)
                {
                    return Err("stop play first");
                }
                if (previewing(ctx.sceneEdit))
                {
                    return Err("exit the asset preview first");
                }
                ProjectInfo project;
                auto result =
                    createProject(ctx.assets, ctx.renderer, ctx.sceneEdit.registry, ctx.sceneEdit.scene, project,
                                  params.name.value_or(""), params.displayName.value_or(""), params.root.value_or(""));
                if (!result)
                {
                    return Err(result.error());
                }
                applyProjectInfo(ctx, project);
                ctx.sceneEdit.sceneVersion += 1;
                ctx.sceneEdit.scriptInputKeys.clear();
                setSelection(ctx.sceneEdit, Entity{ entt::null });
                return projectDto(project);
            });

        registerCommand<CreateScriptParams, CreateScriptResult>(
            reg, "create-script", "create-script {name} — boilerplate .lua under the project src/",
            [](EngineContext& ctx, const CreateScriptParams& params) -> Result<CreateScriptResult>
            {
                if (!ctx.sceneEdit.projectLoaded)
                {
                    return Err(std::string{ "no project loaded" });
                }
                auto created = createProjectScript(ctx.sceneEdit.projectRoot, params.name);
                if (!created)
                {
                    return Err(created.error());
                }
                return CreateScriptResult{ std::move(*created) };
            });

        registerCommand<PathParams, ProjectInfoDto>(
            reg, "open-project", "open-project {path}",
            [](EngineContext& ctx, const PathParams& params) -> Result<ProjectInfoDto>
            {
                if (ctx.sceneEdit.playState != PlayState::Edit)
                {
                    return Err("stop play first");
                }
                if (previewing(ctx.sceneEdit))
                {
                    return Err("exit the asset preview first");
                }
                if (params.path.empty())
                {
                    return Err(std::string{ "missing 'path'" });
                }
                ProjectInfo project;
                nlohmann::json editorCamera;
                auto result = loadProject(ctx.assets, ctx.renderer, ctx.sceneEdit.registry, ctx.sceneEdit.scene,
                                          project, params.path, &editorCamera);
                if (!result)
                {
                    return Err(result.error());
                }
                applyProjectInfo(ctx, project);
                sceneEditCameraFromJson(ctx.sceneEdit.camera, editorCamera);
                ctx.sceneEdit.sceneVersion += 1;
                ctx.sceneEdit.scriptInputKeys.clear();
                setSelection(ctx.sceneEdit, Entity{ entt::null });
                return projectDto(project);
            });

        // Imports a glTF/OBJ by baking it into one .smodel asset + catalog rows, returning the model
        // asset ref. The mesh, materials, and textures are chunks inside the container; instantiate-model
        // places the asset into the scene.
        registerCommand<PathParams, ImportModelResult>(
            reg, "import-model", "import-model {path}",
            [](EngineContext& ctx, const PathParams& params) -> Result<ImportModelResult>
            {
                if (params.path.empty())
                {
                    return Err(std::string{ "missing 'path'" });
                }
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                if (previewing(ctx.sceneEdit))
                {
                    return Err("exit the asset preview first");
                }
                auto bake = importModel(ctx.assets, params.path, ImportOptions{});
                if (!bake)
                {
                    return Err(bake.error());
                }
                std::string name;
                if (const AssetEntry* model = findAsset(ctx.assets.catalog, bake->modelId); model != nullptr)
                {
                    name = model->name;
                }
                return ImportModelResult{ .id = WireUuid{ bake->modelId.value }, .name = name, .type = "model" };
            });

        // Expands a model asset's stored hierarchy into the scene. Returns the new root entity.
        registerCommand<InstantiateModelParams, EntityRef>(
            reg, "instantiate-model", "instantiate-model {asset} [name]",
            [](EngineContext& ctx, const InstantiateModelParams& params) -> Result<EntityRef>
            {
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                auto resolved = resolveAsset(ctx, params.asset);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                const AssetEntry* entry = *resolved;
                if (entry->type != AssetType::Model)
                {
                    return Err(std::format("asset {} is not a model", entry->id.value));
                }
                std::string name = entry->name;
                if (params.name && !params.name->empty())
                {
                    name = *params.name;
                }
                auto root = instantiateModel(activeScene(ctx.sceneEdit), ctx.assets, entry->id, name);
                if (!root)
                {
                    return Err(root.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                setSelection(ctx.sceneEdit, *root);
                return entityRefDto(activeScene(ctx.sceneEdit), *root);
            });

        // Rescans assets/ and reconciles the catalog with disk (the filesystem is the source of truth,
        // so a never-saved import is rediscovered). Idles + clears the GPU caches first; they re-load
        // lazily against the rebuilt catalog. Returns the count of rows added / removed.
        registerCommand<EmptyParams, ScanAssetsResult>(
            reg, "scan-assets", "scan-assets",
            [](EngineContext& ctx, const EmptyParams&) -> Result<ScanAssetsResult>
            {
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                waitGpuIdle(ctx.renderer);
                clearAssetCaches(ctx.assets);
                auto delta = scanAssets(ctx.assets);
                if (!delta)
                {
                    return Err(delta.error());
                }
                writeCatalogCache(ctx.assets);  // refresh the latency cache after a forced rescan
                return ScanAssetsResult{ .added = static_cast<i32>(delta->added.size()),
                                         .removed = static_cast<i32>(delta->removed.size()) };
            });

        // Slices an embedded sub-asset out of its container to a standalone file (same id) + remaps the
        // container to prefer it. Edit/share the extracted file; the embedded chunk stays as a fallback.
        registerCommand<ExtractSubAssetParams, AssetRef>(
            reg, "extract-subasset", "extract-subasset {asset} {subAsset} [dest]",
            [](EngineContext& ctx, const ExtractSubAssetParams& params) -> Result<AssetRef>
            {
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                auto resolved = resolveAsset(ctx, params.asset);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                const Uuid modelId = (*resolved)->id;
                auto extracted =
                    extractSubAsset(ctx.assets, modelId, Uuid{ params.subAsset.value }, params.dest.value_or(""));
                if (!extracted)
                {
                    return Err(extracted.error());
                }
                std::string name;
                if (const AssetEntry* row = findAsset(ctx.assets.catalog, *extracted); row != nullptr)
                {
                    name = row->name;
                }
                return AssetRef{ WireUuid{ extracted->value }, name, std::nullopt };
            });

        // Reverts an extracted sub-asset: drops the remap + external file, back to the embedded chunk.
        registerCommand<ClearExtractionParams, AssetRef>(
            reg, "clear-extraction", "clear-extraction {asset} {subAsset}",
            [](EngineContext& ctx, const ClearExtractionParams& params) -> Result<AssetRef>
            {
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                auto resolved = resolveAsset(ctx, params.asset);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                const Uuid modelId = (*resolved)->id;
                const Uuid subId{ params.subAsset.value };
                if (auto cleared = clearExtraction(ctx.assets, modelId, subId); !cleared)
                {
                    return Err(cleared.error());
                }
                std::string name;
                if (const AssetEntry* row = findAsset(ctx.assets.catalog, subId); row != nullptr)
                {
                    name = row->name;
                }
                return AssetRef{ WireUuid{ subId.value }, name, std::nullopt };
            });

        // Re-bakes a model from its stored source (skips when unchanged), preserving extractions; live
        // instances pick up the new bytes with no re-instantiation. Idles the GPU before patching caches.
        registerCommand<ReimportModelParams, ReimportModelResult>(
            reg, "reimport-model", "reimport-model {asset}",
            [](EngineContext& ctx, const ReimportModelParams& params) -> Result<ReimportModelResult>
            {
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                auto resolved = resolveAsset(ctx, params.asset);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                waitGpuIdle(ctx.renderer);
                auto delta = reimportModel(ctx.assets, (*resolved)->id);
                if (!delta)
                {
                    return Err(delta.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return ReimportModelResult{ .updated = static_cast<i32>(delta->updated.size()),
                                            .added = static_cast<i32>(delta->added.size()),
                                            .removedFromSource = static_cast<i32>(delta->removedFromSource.size()),
                                            .skipped = delta->skipped };
            });

        // A container's metadata summary: sub-asset list (type, name, bytes), material/node/skin counts,
        // source recipe, and total footprint — the Reference-Viewer "what's inside" payload.
        registerCommand<ModelInfoParams, ModelInfoResult>(
            reg, "model-info", "model-info {asset}",
            [](EngineContext& ctx, const ModelInfoParams& params) -> Result<ModelInfoResult>
            {
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                auto resolved = resolveAsset(ctx, params.asset);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                const AssetEntry* entry = *resolved;
                if (entry->type != AssetType::Model)
                {
                    return Err(std::format("asset {} is not a model", entry->id.value));
                }
                auto model = loadModelAsset(ctx.assets, entry->id);
                if (!model)
                {
                    return Err(std::format("model {} is not loadable", entry->id.value));
                }
                ModelInfoResult result;
                result.id = WireUuid{ entry->id.value };
                result.name = model->meta.name;
                result.sourcePath = model->meta.import.sourcePath;
                result.sourceHash = model->meta.import.sourceHash;
                result.hasSkin = !model->meta.skin.is_null();
                result.nodeCount = model->meta.nodes.is_array() ? static_cast<i32>(model->meta.nodes.size()) : 0;
                result.materialCount = 0;
                std::error_code ec;
                result.totalBytes =
                    static_cast<u64>(std::filesystem::file_size(ctx.assets.root + "/" + entry->path, ec));
                for (const auto& sub : model->meta.subAssets)
                {
                    if (sub.type == AssetType::Material)
                    {
                        result.materialCount = result.materialCount + 1;
                    }
                    AssetEntry subRow;
                    subRow.id = sub.subId;
                    subRow.type = sub.type;
                    subRow.container = entry->id;
                    ModelSubAssetDto dto;
                    dto.id = WireUuid{ sub.subId.value };
                    dto.name = sub.name;
                    dto.type = assetTypeName(sub.type);
                    dto.bytes = assetBytes(ctx.assets, subRow);
                    result.subAssets.push_back(std::move(dto));
                }
                return result;
            });

        // What-references-this / what-this-references + footprint, over the live dependency graph.
        registerCommand<AssetReferencesParams, AssetReferencesResult>(
            reg, "asset-references", "asset-references {asset}",
            [](EngineContext& ctx, const AssetReferencesParams& params) -> Result<AssetReferencesResult>
            {
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                auto resolved = resolveAsset(ctx, params.asset);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                const Uuid id = (*resolved)->id;
                DependencyGraph graph =
                    buildDependencyGraph(activeScene(ctx.sceneEdit), ctx.assets.catalog, ctx.assets);
                AssetReferencesResult result;
                for (const Uuid referrer : graph.referencedBy(id))
                {
                    result.referencedBy.push_back(std::to_string(referrer.value));
                }
                for (const Uuid target : graph.referencesOf(id))
                {
                    result.references.push_back(std::to_string(target.value));
                }
                result.footprint = graph.footprint(id);
                return result;
            });

        // The model the asset editor consumes: its capabilities (mesh/material/node counts, rig + clip
        // presence), the node forest as a flat parent-indexed bone tree (joints flagged) when the model
        // carries a skin, and its animation sub-assets as clips — all read from the `.smodel` container
        // metadata. Accepts the model, a mesh sub-asset, or a clip sub-asset; all resolve to the same
        // owning container, so the clip↔mesh link is intrinsic, not a stored field. A skinless (static)
        // model returns an empty bone tree, not an error.
        registerCommand<GetAssetModelParams, AssetModelResult>(
            reg, "get-asset-model",
            "get-asset-model {asset} — a model's capabilities + bone tree + clips, from its .smodel container",
            [](EngineContext& ctx, const GetAssetModelParams& params) -> Result<AssetModelResult>
            {
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                auto resolved = resolveAsset(ctx, params.asset);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                const AssetEntry* entry = *resolved;
                const Uuid containerId = entry->type == AssetType::Model ? entry->id : entry->container;
                if (containerId.value == 0)
                {
                    return Err(std::format("asset {} is not part of a model container", entry->id.value));
                }
                auto model = loadModelAsset(ctx.assets, containerId);
                if (!model)
                {
                    return Err(std::format("model {} is not loadable", containerId.value));
                }
                const ContainerMetadata& meta = model->meta;
                AssetModelResult result;
                result.mesh = WireUuid{ containerId.value };
                result.name = meta.name;
                const std::size_t nodeCount = meta.nodes.is_array() ? meta.nodes.size() : 0;
                const bool hasRig = !meta.skin.is_null();
                // The bone tree, only when the container carries a skin: the joints plus their ancestor
                // chains, bounded at the skeleton root (intermediate non-joint nodes included), but not
                // the mesh node or unrelated scene roots. Node indices are preserved so
                // set-skeleton-highlight stays index-keyed. A static model leaves `bones` empty.
                if (hasRig)
                {
                    std::vector<i32> parents(nodeCount, -1);
                    for (std::size_t i = 0; i < nodeCount; i = i + 1)
                    {
                        parents[i] = meta.nodes[i].value("parent", -1);
                    }
                    std::vector<bool> isJoint(nodeCount, false);
                    i32 skeletonRoot = meta.skin.value("skeletonRoot", -1);
                    if (auto it = meta.skin.find("joints"); it != meta.skin.end() && it->is_array())
                    {
                        for (const json& joint : *it)
                        {
                            const i32 index = joint.get<i32>();
                            if (index >= 0 && static_cast<std::size_t>(index) < nodeCount)
                            {
                                isJoint[static_cast<std::size_t>(index)] = true;
                            }
                        }
                    }
                    std::vector<bool> inRig(nodeCount, false);
                    if (skeletonRoot >= 0 && static_cast<std::size_t>(skeletonRoot) < nodeCount)
                    {
                        inRig[static_cast<std::size_t>(skeletonRoot)] = true;
                    }
                    for (std::size_t i = 0; i < nodeCount; i = i + 1)
                    {
                        if (!isJoint[i])
                        {
                            continue;
                        }
                        i32 node = static_cast<i32>(i);
                        while (node >= 0 && static_cast<std::size_t>(node) < nodeCount &&
                               !inRig[static_cast<std::size_t>(node)])
                        {
                            inRig[static_cast<std::size_t>(node)] = true;
                            if (node == skeletonRoot)
                            {
                                break;
                            }
                            node = parents[static_cast<std::size_t>(node)];
                        }
                    }
                    for (std::size_t i = 0; i < nodeCount; i = i + 1)
                    {
                        if (!inRig[i])
                        {
                            continue;
                        }
                        const i32 parent = parents[i];
                        BoneDto bone;
                        bone.index = static_cast<i32>(i);
                        bone.name = meta.nodes[i].value("name", std::string{});
                        bone.parent = parent >= 0 && static_cast<std::size_t>(parent) < nodeCount &&
                                              inRig[static_cast<std::size_t>(parent)]
                                          ? parent
                                          : -1;
                        bone.joint = isJoint[i];
                        result.bones.push_back(std::move(bone));
                    }
                }
                i32 meshCount = 0;
                i32 materialCount = 0;
                for (const ContainerMetadata::SubAsset& sub : meta.subAssets)
                {
                    if (sub.type == AssetType::Animation)
                    {
                        result.clips.push_back(
                            AnimationClipDto{ WireUuid{ sub.subId.value }, sub.name, sub.duration, sub.tracks });
                    }
                    else if (sub.type == AssetType::Mesh)
                    {
                        meshCount += 1;
                    }
                    else if (sub.type == AssetType::Material)
                    {
                        materialCount += 1;
                    }
                }
                result.capabilities.meshCount = meshCount;
                result.capabilities.materialCount = materialCount;
                result.capabilities.nodeCount = static_cast<i32>(nodeCount);
                result.capabilities.hasRig = hasRig;
                result.capabilities.boneCount = static_cast<i32>(result.bones.size());
                result.capabilities.clipCount = static_cast<i32>(result.clips.size());
                return result;
            });

        // Open any model in an isolated preview scene (the asset editor). Routes through activeScene like
        // play mode, so render / compute skinning / animation / entity-addressed commands all retarget to
        // the preview for free, and the authored scene cannot leak. Accepts the model, a mesh sub-asset,
        // or a clip sub-asset (a clip becomes the active clip). A static (skinless) model previews too —
        // it just has no bone table. Entering while previewing a different model swaps (drop + respawn),
        // not errors. Camera + selection are stashed engine-side, so byte-identity holds for CLI-driven
        // enter/exit too.
        registerCommand<EnterAssetPreviewParams, AssetPreviewResult>(
            reg, "enter-asset-preview", "enter-asset-preview {asset} — open any model in an isolated preview scene",
            [](EngineContext& ctx, const EnterAssetPreviewParams& params) -> Result<AssetPreviewResult>
            {
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                if (ctx.sceneEdit.playState != PlayState::Edit)
                {
                    return Err("stop play first");
                }
                auto resolved = resolveAsset(ctx, params.asset);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                const AssetEntry* entry = *resolved;
                const Uuid containerId = entry->type == AssetType::Model ? entry->id : entry->container;
                if (containerId.value == 0)
                {
                    return Err(std::format("asset {} is not part of a model container", entry->id.value));
                }
                auto model = loadModelAsset(ctx.assets, containerId);
                if (!model)
                {
                    return Err(std::format("model {} is not loadable", containerId.value));
                }
                const ContainerMetadata& meta = model->meta;

                // Build the preview scene locally so a failure leaves the current state untouched (a
                // failed swap stays on the prior model); commit only once the model spawned a renderable
                // mesh (skinned or static).
                Scene preview;
                preview.catalog = ctx.sceneEdit.scene.catalog;
                auto root = instantiateModel(preview, ctx.assets, containerId, meta.name);
                if (!root)
                {
                    return Err(root.error());
                }
                // A skinned model is a container root; its rig (SkinnedMesh/AnimationPlayer) sits on
                // a descendant. Resolve it for the component lookups; a non-skinned model resolves to
                // its own single mesh entity.
                const Entity animatable = animatableDescendant(preview, *root);
                const bool rigged = hasComponent<SkinnedMeshComponent>(preview, animatable);
                if (!rigged && !hasComponent<MeshComponent>(preview, animatable))
                {
                    return Err(std::format("model '{}' has no renderable mesh — re-import the asset", meta.name));
                }

                // Open-from-clip: that clip becomes the active clip; the model opens paused at the rest
                // pose (previewInEdit off until the first transport action arms it — UE5's behavior).
                if (entry->type == AssetType::Animation && hasComponent<AnimationPlayerComponent>(preview, animatable))
                {
                    AnimationPlayerComponent& player = getComponent<AnimationPlayerComponent>(preview, animatable);
                    player.clip = entry->id;
                    player.time = 0.0f;
                    player.playing = false;
                    player.previewInEdit = false;
                }

                AssetPreviewResult result;
                result.rootEntity = WireUuid{ getComponent<IdComponent>(preview, *root).id.value };
                std::vector<Uuid> boneByNode;
                // Map each joint's node index to its spawned entity (SkinnedMeshComponent.bones is in
                // skin-joint order, matching meta.skin["joints"]) — the highlight table the tree drives.
                // A static model has no skin: the bone table + boneByNode stay empty.
                if (rigged)
                {
                    const SkinnedMeshComponent& skin = getComponent<SkinnedMeshComponent>(preview, animatable);
                    const std::vector<Uuid> boneUuids = skin.bones;
                    std::vector<i32> jointNodes;
                    if (auto it = meta.skin.find("joints"); it != meta.skin.end() && it->is_array())
                    {
                        for (const json& joint : *it)
                        {
                            jointNodes.push_back(joint.get<i32>());
                        }
                    }
                    const std::size_t nodeCount = meta.nodes.is_array() ? meta.nodes.size() : 0;
                    boneByNode.assign(nodeCount, Uuid{ 0 });
                    const std::size_t jointCount =
                        jointNodes.size() < boneUuids.size() ? jointNodes.size() : boneUuids.size();
                    for (std::size_t k = 0; k < jointCount; k = k + 1)
                    {
                        const i32 nodeIdx = jointNodes[k];
                        const Uuid uuid = boneUuids[k];
                        if (nodeIdx >= 0 && static_cast<std::size_t>(nodeIdx) < nodeCount && uuid.value != 0)
                        {
                            boneByNode[static_cast<std::size_t>(nodeIdx)] = uuid;
                            result.bones.push_back(BoneEntityDto{ nodeIdx, WireUuid{ uuid.value } });
                        }
                    }
                }

                // Commit. Stash camera + selection + overlay prefs only on a fresh enter (a swap keeps
                // the original authored stash, so exiting always lands back on the authored state). A
                // fresh enter also makes the preview the active view: route the renderer's active view to
                // AssetPreview and flip previewActiveView so activeScene/previewing target it.
                if (!previewing(ctx.sceneEdit))
                {
                    ctx.sceneEdit.savedCamera = ctx.sceneEdit.camera;
                    ctx.sceneEdit.savedSelection = ctx.sceneEdit.selected;
                    ctx.sceneEdit.savedOverlay = ctx.sceneEdit.skeletonOverlay;
                    ctx.sceneEdit.previewActiveView = true;
                    // Seed the preview view's target to the current (scene) size so it renders immediately;
                    // the editor may override it with set-viewport-size {assetPreview}. Read before the
                    // switch, while the scene view is still active.
                    setViewportDesiredSize(ctx.renderer, ViewId::AssetPreview, viewportWidth(ctx.renderer),
                                           viewportHeight(ctx.renderer));
                    setActiveView(ctx.renderer, ViewId::AssetPreview);
                }
                const Entity rootEntity = *root;
                ctx.sceneEdit.previewScene.emplace(std::move(preview));
                ctx.sceneEdit.previewAsset = containerId;
                ctx.sceneEdit.previewRootEntity = rootEntity;
                ctx.sceneEdit.previewBoneByNode = std::move(boneByNode);
                ctx.sceneEdit.previewFloorEntity = Entity{ entt::null };
                // Bones-on is the expected first frame for a rigged model (UE5 Character > Bones); it
                // no-ops for a static model. The highlight starts clear. Furnishing adds floor/light/sky
                // and frames the fly-cam.
                ctx.sceneEdit.skeletonOverlay.show = true;
                ctx.sceneEdit.skeletonOverlay.highlightJoint = -1;
                const PreviewFraming framing = furnishPreviewScene(ctx.sceneEdit, ctx.assets, ctx.renderer, rootEntity);
                result.target = Vec3{ framing.target.x, framing.target.y, framing.target.z };
                result.distance = framing.distance;
                setSelection(ctx.sceneEdit, rootEntity);
                ctx.sceneEdit.sceneVersion += 1;
                ctx.sceneEdit.animationVersion += 1;
                return result;
            });

        registerCommand<EmptyParams, PlayStateResult>(
            reg, "exit-asset-preview",
            "exit-asset-preview — close the asset preview and restore the authored scene + camera",
            [](EngineContext& ctx, const EmptyParams&) -> Result<PlayStateResult>
            {
                // Closing the active preview view returns the renderer to the scene view; a tab-close
                // while the scene view was already active leaves the renderer untouched.
                if (previewing(ctx.sceneEdit))
                {
                    setActiveView(ctx.renderer, ViewId::Scene);
                }
                leaveAssetPreview(ctx.sceneEdit);  // no-op when no preview is alive (idempotent on tab close)
                return PlayStateResult{
                    playStateName(ctx.sceneEdit.playState),           static_cast<i32>(ctx.sceneEdit.playVersion),
                    static_cast<i32>(ctx.sceneEdit.sceneVersion),     ctx.sceneEdit.hadPrimaryCamera,
                    static_cast<i32>(ctx.sceneEdit.animationVersion), WireUuid{ ctx.sceneEdit.previewAsset.value }
                };
            });

        // Switch the active view the engine renders + publishes. Routes renderer.activeView (which selects
        // the render target + shm publisher + temporal state) and, when a preview scene is alive, performs
        // the scene/camera swap so activeScene + ctx.camera follow the chosen view: AssetPreview restores
        // the preview orbit + previewed root + overlay; Scene parks the orbit and restores the authored
        // fly-cam + selection. The swap is a no-op when no preview is alive (plain scene viewport).
        registerCommand<SetActiveViewParams, SetActiveViewResult>(
            reg, "set-active-view", "set-active-view {view} — switch the rendered view (scene | assetPreview)",
            [](EngineContext& ctx, const SetActiveViewParams& params) -> Result<SetActiveViewResult>
            {
                auto viewId = viewIdFromWire(params.view);
                if (!viewId)
                {
                    return Err(viewId.error());
                }
                // Seed an unsized preview target from the current view size before switching, so a
                // direct set-active-view assetPreview (no prior set-viewport-size) still renders.
                const ViewTargets& previewView = ctx.renderer.views[static_cast<std::size_t>(ViewId::AssetPreview)];
                if (*viewId == ViewId::AssetPreview && previewView.desiredWidth == 0)
                {
                    setViewportDesiredSize(ctx.renderer, ViewId::AssetPreview, viewportWidth(ctx.renderer),
                                           viewportHeight(ctx.renderer));
                }
                setActiveView(ctx.renderer, *viewId);
                if (*viewId == ViewId::AssetPreview)
                {
                    activatePreviewView(ctx.sceneEdit);
                }
                else
                {
                    deactivatePreviewView(ctx.sceneEdit);
                }
                return SetActiveViewResult{ viewIdWire(*viewId) };
            });

        // Preview-scene settings (UE5's Preview Scene Settings, v1 = Show Floor). Adds or removes the
        // floor slab live; the preference persists across enter/exit for the session.
        registerCommand<SetAssetPreviewOptionsParams, AssetPreviewOptionsResult>(
            reg, "set-asset-preview-options",
            "set-asset-preview-options {floor?} — preview-scene settings (show floor)",
            [](EngineContext& ctx, const SetAssetPreviewOptionsParams& params) -> Result<AssetPreviewOptionsResult>
            {
                if (!previewing(ctx.sceneEdit))
                {
                    return Err(std::string{ "not in an asset preview" });
                }
                if (params.floor && *params.floor != ctx.sceneEdit.previewShowFloor)
                {
                    ctx.sceneEdit.previewShowFloor = *params.floor;
                    Scene& scene = *ctx.sceneEdit.previewScene;
                    if (!ctx.sceneEdit.previewShowFloor)
                    {
                        if (ctx.sceneEdit.previewFloorEntity.handle != entt::null &&
                            valid(scene, ctx.sceneEdit.previewFloorEntity))
                        {
                            destroyEntity(scene, ctx.sceneEdit.previewFloorEntity);
                        }
                        ctx.sceneEdit.previewFloorEntity = Entity{ entt::null };
                    }
                    else
                    {
                        const PreviewBounds bounds =
                            computePreviewBounds(scene, ctx.assets, ctx.renderer, ctx.sceneEdit.previewRootEntity);
                        ctx.sceneEdit.previewFloorEntity = spawnPreviewFloor(scene, ctx.assets, ctx.renderer, bounds);
                    }
                    ctx.sceneEdit.sceneVersion += 1;
                }
                return AssetPreviewOptionsResult{ ctx.sceneEdit.previewShowFloor };
            });

        // A categorized cleanup report (Unused / Orphaned / Broken / Review) by reachability from the
        // active scene. Always dry-run — it never deletes; delete-unused is the explicit, gated step.
        registerCommand<CleanAssetsParams, CleanReport>(
            reg, "clean-assets", "clean-assets [exclude...]",
            [](EngineContext& ctx, const CleanAssetsParams& params) -> Result<CleanReport>
            {
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                std::vector<Uuid> exclude;
                if (params.exclude)
                {
                    for (const std::string& id : *params.exclude)
                    {
                        exclude.push_back(Uuid{ std::strtoull(id.c_str(), nullptr, 10) });
                    }
                }
                CleanReportData data =
                    analyzeClean(activeScene(ctx.sceneEdit), ctx.assets.catalog, ctx.assets, exclude);
                CleanReport report;
                report.reclaimableBytes = data.reclaimableBytes;
                for (const CleanCandidate& candidate : data.candidates)
                {
                    report.candidates.push_back(CleanCandidateDto{ WireUuid{ candidate.id.value }, candidate.path,
                                                                   cleanCategoryName(candidate.category),
                                                                   candidate.bytes, candidate.reason });
                }
                return report;
            });

        // Deletes only confirmed-unused assets (refusing without confirm), then rescans for cascade.
        // Outward-facing + irreversible — commit to VCS first.
        registerCommand<DeleteUnusedParams, DeleteUnusedResult>(
            reg, "delete-unused", "delete-unused {ids...} {confirm}",
            [](EngineContext& ctx, const DeleteUnusedParams& params) -> Result<DeleteUnusedResult>
            {
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                std::vector<Uuid> ids;
                for (const std::string& id : params.ids)
                {
                    ids.push_back(Uuid{ std::strtoull(id.c_str(), nullptr, 10) });
                }
                waitGpuIdle(ctx.renderer);
                clearAssetCaches(ctx.assets);
                auto deleted =
                    deleteUnused(ctx.assets, activeScene(ctx.sceneEdit), ids, params.confirm.value_or(false));
                if (!deleted)
                {
                    return Err(deleted.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return DeleteUnusedResult{ .deleted = deleted->deleted, .reclaimedBytes = deleted->reclaimedBytes };
            });

        // Imports an external image into the asset dir; returns its texture id (assign
        // it with set-material --albedoTexture <id>).
        registerCommand<PathParams, ImportTextureResult>(
            reg, "import-texture", "import-texture {path}",
            [](EngineContext& ctx, const PathParams& params) -> Result<ImportTextureResult>
            {
                if (params.path.empty())
                {
                    return Err(std::string{ "missing 'path'" });
                }
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                auto id = importTexture(ctx.assets, ctx.renderer, params.path);
                if (!id)
                {
                    return Err(id.error());
                }
                return ImportTextureResult{ WireUuid{ id->value } };
            });

        registerCommand<EmptyParams, AssetList>(reg, "list-assets", "list the project asset catalog",
                                                [](EngineContext& ctx, const EmptyParams&) -> Result<AssetList>
                                                { return assetListDto(ctx.assets.catalog); });

        registerCommand<RenameAssetParams, AssetRef>(
            reg, "rename-asset", "rename-asset {id|name, newName}",
            [](EngineContext& ctx, const RenameAssetParams& params) -> Result<AssetRef>
            {
                const std::string selector =
                    params.asset.value.is_string() ? params.asset.value.get<std::string>() : std::string{};
                if (selector.empty() || params.name.empty())
                {
                    return Err(std::string{ "usage: rename-asset {id|name} {newName}" });
                }
                const u64 byId = std::strtoull(selector.c_str(), nullptr, 10);
                for (AssetEntry& entry : ctx.assets.catalog.entries)
                {
                    if (entry.id.value == byId || entry.name == selector)
                    {
                        entry.name = params.name;
                        return assetRef(entry);
                    }
                }
                return Err(std::format("no asset '{}'", selector));
            });

        registerCommand<CreateAssetFolderParams, AssetList>(
            reg, "create-asset-folder", "create-asset-folder {folder}",
            [](EngineContext& ctx, const CreateAssetFolderParams& params) -> Result<AssetList>
            {
                if (!validFolderPath(params.folder))
                {
                    return Err(std::string{ "folder must be a non-empty path without empty segments" });
                }
                if (!hasFolder(ctx.assets.catalog, params.folder))
                {
                    ctx.assets.catalog.folders.push_back(params.folder);
                    ctx.sceneEdit.sceneVersion += 1;
                }
                return assetListDto(ctx.assets.catalog);
            });

        registerCommand<RenameAssetFolderParams, AssetList>(
            reg, "rename-asset-folder", "rename-asset-folder {folder, name}",
            [](EngineContext& ctx, const RenameAssetFolderParams& params) -> Result<AssetList>
            {
                if (!validFolderPath(params.name))
                {
                    return Err(std::string{ "folder path must be non-empty and cannot contain empty segments" });
                }
                if (!hasFolder(ctx.assets.catalog, params.folder))
                {
                    return Err(std::format("no asset folder '{}'", params.folder));
                }
                if (params.folder == params.name)
                {
                    return assetListDto(ctx.assets.catalog);
                }
                if (isFolderDescendant(params.name, params.folder))
                {
                    return Err(std::string{ "asset folder cannot be moved inside itself" });
                }
                if (hasFolder(ctx.assets.catalog, params.name))
                {
                    return Err(std::format("asset folder '{}' already exists", params.name));
                }
                for (std::string& folder : ctx.assets.catalog.folders)
                {
                    if (folder == params.folder || isFolderDescendant(folder, params.folder))
                    {
                        folder = replaceFolderPrefix(folder, params.folder, params.name);
                    }
                }
                for (AssetEntry& entry : ctx.assets.catalog.entries)
                {
                    if (entry.folder == params.folder || isFolderDescendant(entry.folder, params.folder))
                    {
                        entry.folder = replaceFolderPrefix(entry.folder, params.folder, params.name);
                    }
                }
                ctx.sceneEdit.sceneVersion += 1;
                return assetListDto(ctx.assets.catalog);
            });

        registerCommand<DeleteAssetFolderParams, AssetList>(
            reg, "delete-asset-folder", "delete-asset-folder {folder}",
            [](EngineContext& ctx, const DeleteAssetFolderParams& params) -> Result<AssetList>
            {
                bool removed = false;
                std::vector<std::string> folders;
                folders.reserve(ctx.assets.catalog.folders.size());
                for (const std::string& folder : ctx.assets.catalog.folders)
                {
                    if (folder == params.folder || isFolderDescendant(folder, params.folder))
                    {
                        removed = true;
                    }
                    else
                    {
                        folders.push_back(folder);
                    }
                }
                if (!removed)
                {
                    return Err(std::format("no asset folder '{}'", params.folder));
                }
                ctx.assets.catalog.folders = std::move(folders);
                for (AssetEntry& entry : ctx.assets.catalog.entries)
                {
                    if (entry.folder == params.folder || isFolderDescendant(entry.folder, params.folder))
                    {
                        entry.folder.clear();
                    }
                }
                ctx.sceneEdit.sceneVersion += 1;
                return assetListDto(ctx.assets.catalog);
            });

        registerCommand<MoveAssetParams, AssetRef>(
            reg, "move-asset", "move-asset {asset, folder?}",
            [](EngineContext& ctx, const MoveAssetParams& params) -> Result<AssetRef>
            {
                auto index = resolveAssetIndex(ctx, params.asset);
                if (!index)
                {
                    return Err(index.error());
                }
                std::string folder = params.folder.value_or("");
                if (!folder.empty() && !hasFolder(ctx.assets.catalog, folder))
                {
                    return Err(std::format("no asset folder '{}'", folder));
                }
                AssetEntry& entry = ctx.assets.catalog.entries[*index];
                entry.folder = std::move(folder);
                ctx.sceneEdit.sceneVersion += 1;
                return assetRef(entry);
            });

        registerCommand<AssetUsagesParams, AssetUsagesResult>(
            reg, "asset-usages", "asset-usages {asset}",
            [](EngineContext& ctx, const AssetUsagesParams& params) -> Result<AssetUsagesResult>
            {
                auto resolved = resolveAsset(ctx, params.asset);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                return AssetUsagesResult{ collectAssetUsages(activeScene(ctx.sceneEdit), (*resolved)->id) };
            });

        registerCommand<AssetMetadataParams, AssetMetadataDto>(
            reg, "probe-asset", "probe-asset {asset}",
            [](EngineContext& ctx, const AssetMetadataParams& params) -> Result<AssetMetadataDto>
            {
                auto resolved = resolveAsset(ctx, params.asset);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                const AssetEntry& entry = **resolved;
                const std::filesystem::path abs = std::filesystem::path(ctx.assets.root) / entry.path;

                u64 sizeBytes = 0;
                {
                    std::error_code ec;
                    const auto size = std::filesystem::file_size(abs, ec);
                    if (!ec)
                    {
                        sizeBytes = static_cast<u64>(size);
                    }
                }

                i64 createdAt = 0;
                {
                    std::error_code ec;
                    const auto ftime = std::filesystem::last_write_time(abs, ec);
                    if (!ec)
                    {
                        const auto sys = std::chrono::file_clock::to_sys(ftime);
                        createdAt = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
                    }
                }

                std::optional<u32> vertexCount;
                std::optional<u32> triangleCount;
                if (entry.type == AssetType::Mesh)
                {
                    if (auto counts = meshFileCounts(abs.string()))
                    {
                        vertexCount = counts->vertexCount;
                        triangleCount = counts->indexCount / 3;
                    }
                }

                return AssetMetadataDto{ WireUuid{ entry.id.value },
                                         entry.name,
                                         assetTypeDto(entry.type),
                                         entry.path,
                                         entry.folder.empty() ? std::optional<std::string>{}
                                                              : std::optional<std::string>{ entry.folder },
                                         sizeBytes,
                                         vertexCount,
                                         triangleCount,
                                         createdAt };
            });

        registerCommand<DeleteAssetParams, DeleteAssetResult>(
            reg, "delete-asset", "delete-asset {asset}",
            [](EngineContext& ctx, const DeleteAssetParams& params) -> Result<DeleteAssetResult>
            {
                // Guarded rather than routed: delete clears component usages *and* drops the
                // GPU ref the play scene renders from, so it must wait for edit.
                if (ctx.sceneEdit.playState != PlayState::Edit)
                {
                    return Err("stop play first");
                }
                if (previewing(ctx.sceneEdit))
                {
                    return Err("exit the asset preview first");
                }
                auto index = resolveAssetIndex(ctx, params.asset);
                if (!index)
                {
                    return Err(index.error());
                }
                AssetEntry entry = ctx.assets.catalog.entries[*index];
                std::vector<AssetUsageDto> cleared = clearAssetUsages(ctx.sceneEdit.scene, entry.id);
                ctx.assets.catalog.entries.erase(ctx.assets.catalog.entries.begin() +
                                                 static_cast<std::ptrdiff_t>(*index));
                rebuildAssetIndex(ctx.assets.catalog);
                ctx.assets.meshRefByUuid.erase(entry.id.value);
                ctx.assets.textureRefByUuid.erase(entry.id.value);

                bool fileDeleted = false;
                if (!entry.path.empty())
                {
                    const std::filesystem::path path = std::filesystem::path(ctx.assets.root) / entry.path;
                    std::error_code ec;
                    fileDeleted = std::filesystem::remove(path, ec);
                }
                removeThumbnailCacheForAsset(ctx.assets, entry.id);  // drop the asset's cached PNGs
                ctx.sceneEdit.sceneVersion += 1;
                return DeleteAssetResult{ WireUuid{ entry.id.value }, entry.name, std::move(cleared), fileDeleted };
            });

        registerCommand<AssignAssetParams, AssignAssetResult>(
            reg, "assign-asset", "assign-asset {entity, slot:mesh|albedo|metallic-roughness, id|name}",
            [](EngineContext& ctx, const AssignAssetParams& params) -> Result<AssignAssetResult>
            {
                if (previewing(ctx.sceneEdit))
                {
                    return Err("exit the asset preview first");
                }
                auto entity = resolveEntity(ctx, json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                // The null sentinel (id 0 / empty selector) clears the slot rather than
                // resolving an asset, so the editor's "(none)" choice unassigns mesh/albedo.
                const json& sel = params.asset.value;
                const std::string selector = sel.is_string() ? sel.get<std::string>() : std::string{};
                const bool clearing = selector == "0" || selector.empty() ||
                                      (sel.is_number_unsigned() && sel.get<u64>() == 0) ||
                                      (sel.is_number_integer() && sel.get<i64>() == 0);
                Uuid assignId{ 0 };
                std::string assignName;
                if (!clearing)
                {
                    auto resolved = resolveAsset(ctx, params.asset);
                    if (!resolved)
                    {
                        return Err(resolved.error());
                    }
                    assignId = (*resolved)->id;
                    assignName = (*resolved)->name;
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                if (params.slot == AssetSlotDto::Mesh)
                {
                    if (!hasComponent<MeshComponent>(scene, *entity))
                    {
                        addComponent<MeshComponent>(scene, *entity);
                    }
                    getComponent<MeshComponent>(scene, *entity).mesh = assignId;
                }
                else if (params.slot == AssetSlotDto::Albedo)
                {
                    if (!hasComponent<MaterialComponent>(scene, *entity))
                    {
                        addComponent<MaterialComponent>(scene, *entity);
                    }
                    getComponent<MaterialComponent>(scene, *entity).albedoTexture = assignId;
                }
                else if (params.slot == AssetSlotDto::MetallicRoughness)
                {
                    if (!hasComponent<MaterialComponent>(scene, *entity))
                    {
                        addComponent<MaterialComponent>(scene, *entity);
                    }
                    getComponent<MaterialComponent>(scene, *entity).metallicRoughnessTexture = assignId;
                }
                else if (params.slot == AssetSlotDto::Normal)
                {
                    if (!hasComponent<MaterialComponent>(scene, *entity))
                    {
                        addComponent<MaterialComponent>(scene, *entity);
                    }
                    getComponent<MaterialComponent>(scene, *entity).normalTexture = assignId;
                }
                else if (params.slot == AssetSlotDto::Occlusion)
                {
                    if (!hasComponent<MaterialComponent>(scene, *entity))
                    {
                        addComponent<MaterialComponent>(scene, *entity);
                    }
                    getComponent<MaterialComponent>(scene, *entity).occlusionTexture = assignId;
                }
                else if (params.slot == AssetSlotDto::Emissive)
                {
                    if (!hasComponent<MaterialComponent>(scene, *entity))
                    {
                        addComponent<MaterialComponent>(scene, *entity);
                    }
                    getComponent<MaterialComponent>(scene, *entity).emissiveTexture = assignId;
                }
                else if (params.slot == AssetSlotDto::Height)
                {
                    if (!hasComponent<MaterialComponent>(scene, *entity))
                    {
                        addComponent<MaterialComponent>(scene, *entity);
                    }
                    getComponent<MaterialComponent>(scene, *entity).heightTexture = assignId;
                }
                ctx.sceneEdit.sceneVersion += 1;
                return AssignAssetResult{ WireUuid{ assignId.value }, assignName, params.slot };
            });

        registerCommand<MaterialCreateParams, MaterialCreateResult>(
            reg, "material-create", "material-create {name} [from-entity]",
            [](EngineContext& ctx, const MaterialCreateParams& params) -> Result<MaterialCreateResult>
            {
                MaterialAsset asset;
                const std::string name = params.name.empty() ? std::string{ "Material" } : params.name;
                auto id = saveMaterialAsset(ctx.assets, asset, name);
                if (!id)
                {
                    return Err(id.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return MaterialCreateResult{ WireUuid{ id->value }, name };
            });

        registerCommand<MaterialAssignParams, MaterialAssignResult>(
            reg, "material-assign", "material-assign {entity, material:id|name}",
            [](EngineContext& ctx, const MaterialAssignParams& params) -> Result<MaterialAssignResult>
            {
                auto entity = resolveEntity(ctx, json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                const json& sel = params.material.value;
                const std::string selector = sel.is_string() ? sel.get<std::string>() : std::string{};
                const bool clearing =
                    selector == "0" || selector.empty() || (sel.is_number_unsigned() && sel.get<u64>() == 0);
                Uuid matId{ 0 };
                if (!clearing)
                {
                    auto resolved = resolveAsset(ctx, params.material);
                    if (!resolved)
                    {
                        return Err(resolved.error());
                    }
                    matId = (*resolved)->id;
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                if (!hasComponent<MaterialAssetComponent>(scene, *entity))
                {
                    addComponent<MaterialAssetComponent>(scene, *entity);
                }
                getComponent<MaterialAssetComponent>(scene, *entity).material = matId;  // 0 = cleared
                ctx.sceneEdit.sceneVersion += 1;
                return MaterialAssignResult{ WireUuid{ matId.value } };
            });

        registerCommand<EmptyParams, MaterialCookResult>(
            reg, "material-cook", "material-cook",
            [](EngineContext& ctx, const EmptyParams&) -> Result<MaterialCookResult>
            {
                // Bake every codegen material's übershader variant to disk (the shipping/precompile
                // direction): a non-foldable graph needs its per-material shader compiled. Foldable and
                // graphless materials are skipped (they draw on the shared übershader).
                MaterialCookResult out{ 0, 0 };
                for (const AssetEntry& entry : ctx.assets.catalog.entries)
                {
                    if (entry.type != AssetType::Material)
                    {
                        continue;
                    }
                    auto raw = loadMaterialAssetRaw(ctx.assets, entry.id);
                    if (!raw || !raw->graph.is_object() || raw->graph.empty())
                    {
                        continue;
                    }
                    MaterialAsset probe = *raw;
                    if (lowerGraphToParams(raw->graph, probe))
                    {
                        continue;  // folds to params — no shader needed
                    }
                    if (compileMaterialMeshShader(ctx.assets, raw->graph, entry.id))
                    {
                        out.compiled += 1;
                    }
                    else
                    {
                        out.failed += 1;
                    }
                }
                return out;
            });

        registerCommand<MaterialCompileParams, MaterialCompileResult>(
            reg, "material-compile-graph", "material-compile-graph {material}",
            [](EngineContext& ctx, const MaterialCompileParams& params) -> Result<MaterialCompileResult>
            {
                auto resolved = resolveAsset(ctx, params.material);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                auto raw = loadMaterialAssetRaw(ctx.assets, (*resolved)->id);
                if (!raw)
                {
                    return Err(raw.error());
                }
                if (!raw->graph.is_object() || raw->graph.empty())
                {
                    return Err(std::string{ "material has no node graph to compile" });
                }
                auto spv = compileMaterialGraph(ctx.assets, raw->graph, (*resolved)->id);
                if (!spv)
                {
                    return Err(spv.error());
                }
                return MaterialCompileResult{ WireUuid{ (*resolved)->id.value }, true };
            });

        registerCommand<MaterialImportParams, MaterialImportResultDto>(
            reg, "material-import", "material-import {path} [name]",
            [](EngineContext& ctx, const MaterialImportParams& params) -> Result<MaterialImportResultDto>
            {
                auto result = importMaterialFolder(ctx.assets, ctx.renderer, params.path, params.name);
                if (!result)
                {
                    return Err(result.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return MaterialImportResultDto{ WireUuid{ result->material.value }, result->roles };
            });

        registerCommand<EmptyParams, MaterialListResult>(
            reg, "material-list", "material-list",
            [](EngineContext& ctx, const EmptyParams&) -> Result<MaterialListResult>
            {
                MaterialListResult out;
                for (const AssetEntry& entry : ctx.assets.catalog.entries)
                {
                    if (entry.type == AssetType::Material)
                    {
                        out.materials.push_back(MaterialRefDto{ WireUuid{ entry.id.value }, entry.name, entry.folder });
                    }
                }
                return out;
            });

        registerCommand<MaterialGetParams, MaterialGetResult>(
            reg, "material-get", "material-get {id|name}",
            [](EngineContext& ctx, const MaterialGetParams& params) -> Result<MaterialGetResult>
            {
                auto resolved = resolveAsset(ctx, params.material);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                auto loaded = loadMaterialAsset(ctx.assets, (*resolved)->id);
                if (!loaded)
                {
                    return Err(loaded.error());
                }
                const MaterialAsset& m = *loaded;
                MaterialGetResult r;
                r.id = WireUuid{ (*resolved)->id.value };
                r.blend = m.blend;
                r.unlit = m.unlit;
                r.baseColor = Vec4{ m.baseColor.x, m.baseColor.y, m.baseColor.z, m.baseColor.w };
                r.metallic = m.metallic;
                r.roughness = m.roughness;
                r.emissive = Vec3{ m.emissive.x, m.emissive.y, m.emissive.z };
                r.emissiveStrength = m.emissiveStrength;
                r.albedoTexture = WireUuid{ m.albedoTexture.value };
                r.ormTexture = WireUuid{ m.ormTexture.value };
                r.normalTexture = WireUuid{ m.normalTexture.value };
                r.emissiveTexture = WireUuid{ m.emissiveTexture.value };
                r.heightTexture = WireUuid{ m.heightTexture.value };
                // The stored (unfolded) graph is the editor's source of truth; loadMaterialAsset folds it,
                // so read raw. Empty object when the material has no graph.
                if (auto raw = loadMaterialAssetRaw(ctx.assets, (*resolved)->id); raw && raw->graph.is_object())
                {
                    r.graph = raw->graph;
                }
                else
                {
                    r.graph = nlohmann::json::object();
                }
                return r;
            });

        registerCommand<MaterialUpdateParams, MaterialUpdateResult>(
            reg, "material-update", "material-update {id} [baseColor metallic roughness emissive emissiveStrength]",
            [](EngineContext& ctx, const MaterialUpdateParams& params) -> Result<MaterialUpdateResult>
            {
                auto resolved = resolveAsset(ctx, params.material);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                auto loaded = loadMaterialAsset(ctx.assets, (*resolved)->id);
                if (!loaded)
                {
                    return Err(loaded.error());
                }
                MaterialAsset m = *loaded;
                if (params.baseColor)
                {
                    m.baseColor.x = params.baseColor->x;
                    m.baseColor.y = params.baseColor->y;
                    m.baseColor.z = params.baseColor->z;
                    m.baseColor.w = params.baseColor->w;
                }
                if (params.metallic)
                {
                    m.metallic = *params.metallic;
                }
                if (params.roughness)
                {
                    m.roughness = *params.roughness;
                }
                if (params.emissive)
                {
                    m.emissive.x = params.emissive->x;
                    m.emissive.y = params.emissive->y;
                    m.emissive.z = params.emissive->z;
                }
                if (params.emissiveStrength)
                {
                    m.emissiveStrength = *params.emissiveStrength;
                }
                if (params.normalStrength)
                {
                    m.normalStrength = *params.normalStrength;
                }
                if (params.albedoTexture)
                {
                    m.albedoTexture = Uuid{ params.albedoTexture->value };
                }
                if (params.ormTexture)
                {
                    m.ormTexture = Uuid{ params.ormTexture->value };
                }
                if (params.normalTexture)
                {
                    m.normalTexture = Uuid{ params.normalTexture->value };
                }
                if (params.emissiveTexture)
                {
                    m.emissiveTexture = Uuid{ params.emissiveTexture->value };
                }
                if (params.heightTexture)
                {
                    m.heightTexture = Uuid{ params.heightTexture->value };
                }
                if (auto ok = updateMaterialAsset(ctx.assets, (*resolved)->id, m); !ok)
                {
                    return Err(ok.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return MaterialUpdateResult{ WireUuid{ (*resolved)->id.value } };
            });

        registerCommand<PreviewRenderParams, PreviewRenderResult>(
            reg, "preview-render", "preview-render {material} [size]",
            [](EngineContext& ctx, const PreviewRenderParams& params) -> Result<PreviewRenderResult>
            {
                auto resolved = resolveAsset(ctx, params.material);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                auto loaded = loadMaterialAsset(ctx.assets, (*resolved)->id);
                if (!loaded)
                {
                    return Err(loaded.error());
                }
                const SubmeshMaterial sm = resolveMaterialAsset(ctx.assets, ctx.renderer, *loaded);
                const u32 size = params.size.value_or(256u);
                // A non-foldable graph (procedural nodes) renders via a codegen'd preview shader; a
                // foldable graph already folded into sm, so the default studio preview shows it.
                std::string codegenSpv;
                if (auto rawLoaded = loadMaterialAssetRaw(ctx.assets, (*resolved)->id);
                    rawLoaded && rawLoaded->graph.is_object() && !rawLoaded->graph.empty())
                {
                    MaterialAsset probe = *rawLoaded;
                    if (!lowerGraphToParams(rawLoaded->graph, probe))
                    {
                        if (auto spv = compileMaterialPreviewShader(ctx.assets, rawLoaded->graph, (*resolved)->id))
                        {
                            codegenSpv = *spv;
                        }
                    }
                }
                auto tex = renderMaterialPreview(ctx.renderer, sm, size, codegenSpv);
                if (!tex)
                {
                    return Err(tex.error());
                }
                auto png = encodeTextureThumbnailPng(ctx.renderer, *tex, size);
                if (!png)
                {
                    return Err(png.error());
                }
                return PreviewRenderResult{ base64Encode(png->bytes) };
            });

        registerCommand<MaterialSetGraphParams, MaterialSetGraphResult>(
            reg, "material-set-graph", "material-set-graph {material, graph}",
            [](EngineContext& ctx, const MaterialSetGraphParams& params) -> Result<MaterialSetGraphResult>
            {
                auto resolved = resolveAsset(ctx, params.material);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                auto loaded = loadMaterialAsset(ctx.assets, (*resolved)->id);
                if (!loaded)
                {
                    return Err(loaded.error());
                }
                MaterialAsset m = *loaded;
                m.graph = params.graph;
                // Fold the graph into the params (the source of truth) when it has no codegen-only node;
                // report foldability so the editor knows whether the codegen path will be needed.
                MaterialAsset folded = m;
                const bool foldable = lowerGraphToParams(m.graph, folded);
                if (foldable)
                {
                    m = folded;
                }
                if (auto ok = updateMaterialAsset(ctx.assets, (*resolved)->id, m); !ok)
                {
                    return Err(ok.error());
                }
                // A non-foldable graph renders on scene entities via a compiled übershader variant; build
                // it now so resolveEntityMaterials finds it on disk. Failure is non-fatal — the material
                // falls back to the shared übershader.
                if (!foldable)
                {
                    (void)compileMaterialMeshShader(ctx.assets, m.graph, (*resolved)->id);
                }
                ctx.sceneEdit.sceneVersion += 1;
                return MaterialSetGraphResult{ WireUuid{ (*resolved)->id.value }, foldable };
            });

        registerCommand<MaterialCreateInstanceParams, MaterialCreateResult>(
            reg, "material-create-instance", "material-create-instance {parent} [name]",
            [](EngineContext& ctx, const MaterialCreateInstanceParams& params) -> Result<MaterialCreateResult>
            {
                auto parent = resolveAsset(ctx, params.parent);
                if (!parent)
                {
                    return Err(parent.error());
                }
                MaterialAsset child;
                child.parent = (*parent)->id;
                const std::string name = params.name.empty() ? std::string{ "Instance" } : params.name;
                auto id = saveMaterialAsset(ctx.assets, child, name);
                if (!id)
                {
                    return Err(id.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return MaterialCreateResult{ WireUuid{ id->value }, name };
            });

        registerCommand<MaterialSetOverrideParams, MaterialSetOverrideResult>(
            reg, "material-set-override", "material-set-override {material, field, value}",
            [](EngineContext& ctx, const MaterialSetOverrideParams& params) -> Result<MaterialSetOverrideResult>
            {
                auto resolved = resolveAsset(ctx, params.material);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                auto raw = loadMaterialAssetRaw(ctx.assets, (*resolved)->id);
                if (!raw)
                {
                    return Err(raw.error());
                }
                MaterialAsset m = *raw;
                m.overrides[params.field] = params.value;  // null overrides becomes an object on []
                if (auto ok = updateMaterialAsset(ctx.assets, (*resolved)->id, m); !ok)
                {
                    return Err(ok.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return MaterialSetOverrideResult{ WireUuid{ (*resolved)->id.value } };
            });

        registerCommand<PathParams, PathResult>(reg, "save-scene", "save-scene {path}",
                                                [](EngineContext& ctx, const PathParams& params) -> Result<PathResult>
                                                {
                                                    if (params.path.empty())
                                                    {
                                                        return Err(std::string{ "missing 'path'" });
                                                    }
                                                    auto result = writeScene(ctx.sceneEdit.registry,
                                                                             ctx.sceneEdit.scene, params.path);
                                                    if (!result)
                                                    {
                                                        return Err(result.error());
                                                    }
                                                    ctx.sceneEdit.scenePath = params.path;
                                                    return PathResult{ params.path };
                                                });

        registerCommand<PathParams, PathResult>(reg, "load-scene", "load-scene {path}",
                                                [](EngineContext& ctx, const PathParams& params) -> Result<PathResult>
                                                {
                                                    if (ctx.sceneEdit.playState != PlayState::Edit)
                                                    {
                                                        return Err("stop play first");
                                                    }
                                                    if (params.path.empty())
                                                    {
                                                        return Err(std::string{ "missing 'path'" });
                                                    }
                                                    auto result = readScene(ctx.sceneEdit.registry, ctx.sceneEdit.scene,
                                                                            params.path);
                                                    if (!result)
                                                    {
                                                        return Err(result.error());
                                                    }
                                                    ctx.sceneEdit.scenePath = params.path;
                                                    ctx.sceneEdit.sceneVersion += 1;
                                                    setSelection(ctx.sceneEdit, Entity{ entt::null });
                                                    return PathResult{ params.path };
                                                });

        registerCommand<OptionalPathParams, ProjectInfoDto>(
            reg, "save-project", "save-project {path} — assets catalog + scene in one file",
            [](EngineContext& ctx, const OptionalPathParams& params) -> Result<ProjectInfoDto>
            {
                std::string path = params.path.value_or("");
                ProjectInfo project = currentProjectInfo(ctx);
                if (path.empty())
                {
                    path = project.path;
                }
                if (path.empty())
                {
                    return Err(std::string{ "no active project path" });
                }
                if (!project.loaded)
                {
                    const std::filesystem::path fsPath{ path };
                    project.loaded = true;
                    project.path = path;
                    project.root = fsPath.parent_path().empty() ? "." : fsPath.parent_path().string();
                    project.name = validProjectName(fsPath.parent_path().filename().string())
                                       ? fsPath.parent_path().filename().string()
                                       : "project";
                    project.displayName = defaultDisplayName(project.name);
                }
                auto result = saveProject(ctx.assets, ctx.renderer, ctx.sceneEdit.registry, ctx.sceneEdit.scene,
                                          project, path, sceneEditCameraToJson(ctx.sceneEdit.camera));
                if (!result)
                {
                    return Err(result.error());
                }
                project.path = path;
                applyProjectInfo(ctx, project);
                return projectDto(project);
            });

        registerCommand<OptionalPathParams, ProjectInfoDto>(
            reg, "load-project", "load-project {path} — assets catalog + scene",
            [](EngineContext& ctx, const OptionalPathParams& params) -> Result<ProjectInfoDto>
            {
                if (ctx.sceneEdit.playState != PlayState::Edit)
                {
                    return Err("stop play first");
                }
                if (previewing(ctx.sceneEdit))
                {
                    return Err("exit the asset preview first");
                }
                const std::string path = params.path.value_or("project.json");
                ProjectInfo project;
                nlohmann::json editorCamera;
                Result<void> result = loadProject(ctx.assets, ctx.renderer, ctx.sceneEdit.registry, ctx.sceneEdit.scene,
                                                  project, path, &editorCamera);
                if (!result)
                {
                    return Err(result.error());
                }
                applyProjectInfo(ctx, project);
                sceneEditCameraFromJson(ctx.sceneEdit.camera, editorCamera);
                ctx.sceneEdit.sceneVersion += 1;
                ctx.sceneEdit.scriptInputKeys.clear();
                setSelection(ctx.sceneEdit, Entity{ entt::null });
                return projectDto(project);
            });

        // Closes the active project and loads it again from its own path — a clean reload
        // (catalog + scene + GPU assets) without restarting the host process.
        registerCommand<EmptyParams, ProjectInfoDto>(
            reg, "reload-project", "reload-project — close and re-open the active project",
            [](EngineContext& ctx, const EmptyParams&) -> Result<ProjectInfoDto>
            {
                if (ctx.sceneEdit.playState != PlayState::Edit)
                {
                    return Err("stop play first");
                }
                if (previewing(ctx.sceneEdit))
                {
                    return Err("exit the asset preview first");
                }
                if (auto ready = requireProjectLoaded(ctx); !ready)
                {
                    return Err(ready.error());
                }
                const std::string path = ctx.sceneEdit.projectPath;
                ProjectInfo project;
                nlohmann::json editorCamera;
                Result<void> result = loadProject(ctx.assets, ctx.renderer, ctx.sceneEdit.registry, ctx.sceneEdit.scene,
                                                  project, path, &editorCamera);
                if (!result)
                {
                    return Err(result.error());
                }
                applyProjectInfo(ctx, project);
                sceneEditCameraFromJson(ctx.sceneEdit.camera, editorCamera);
                ctx.sceneEdit.sceneVersion += 1;
                ctx.sceneEdit.scriptInputKeys.clear();
                setSelection(ctx.sceneEdit, Entity{ entt::null });
                return projectDto(project);
            });

        registerCommand<ScreenshotParams, ScreenshotResult>(
            reg, "screenshot", "screenshot {target:viewport|window, path}",
            [](EngineContext& ctx, const ScreenshotParams& params) -> Result<ScreenshotResult>
            {
                const ScreenshotTargetDto target = params.target.value_or(ScreenshotTargetDto::Viewport);
                if (params.path.empty())
                {
                    return Err(std::string{ "missing 'path'" });
                }
                if (target == ScreenshotTargetDto::Viewport)
                {
                    auto shot = captureViewport(ctx.renderer, params.path);
                    if (!shot)
                    {
                        return Err(shot.error());
                    }
                    return ScreenshotResult{ target, params.path, false };
                }
                if (target == ScreenshotTargetDto::Window)
                {
                    // Written at the end of the current frame.
                    auto shot = requestWindowCapture(ctx.renderer, params.path);
                    if (!shot)
                    {
                        return Err(shot.error());
                    }
                    return ScreenshotResult{ target, params.path, true };
                }
                return Err(std::format("unknown target '{}' (viewport|window)", screenshotTargetName(target)));
            });

        registerCommand<ThumbnailParams, ThumbnailResult>(
            reg, "get-thumbnail", "get-thumbnail {asset:id|name, size=128} — base64 PNG preview",
            [](EngineContext& ctx, const ThumbnailParams& params) -> Result<ThumbnailResult>
            { return thumbnailResult(ctx, params, 128); });

        registerCommand<ThumbnailParams, ThumbnailResult>(
            reg, "view-asset", "view-asset {asset:id|name, size=512} — larger base64 PNG preview",
            [](EngineContext& ctx, const ThumbnailParams& params) -> Result<ThumbnailResult>
            { return thumbnailResult(ctx, params, 512); });

        registerCommand<ThumbnailCacheParams, ThumbnailCacheResult>(
            reg, "thumbnail-cache", "thumbnail-cache {action: stats|clear} — inspect or empty the disk cache",
            [](EngineContext& ctx, const ThumbnailCacheParams& params) -> Result<ThumbnailCacheResult>
            {
                if (params.action == "clear")
                {
                    const ThumbnailCacheStats removed = clearThumbnailCacheDir(ctx.assets);
                    return ThumbnailCacheResult{ static_cast<i32>(removed.entries), static_cast<i64>(removed.bytes) };
                }
                if (params.action == "stats" || params.action.empty())
                {
                    const ThumbnailCacheStats stats = thumbnailCacheStats(ctx.assets);
                    return ThumbnailCacheResult{ static_cast<i32>(stats.entries), static_cast<i64>(stats.bytes) };
                }
                return Err(std::format("unknown action '{}' (stats|clear)", params.action));
            });

        registerCommand<EmptyParams, QuitResult>(reg, "quit", "close the running app",
                                                 [](EngineContext& ctx, const EmptyParams&) -> Result<QuitResult>
                                                 {
                                                     ctx.window.shouldClose = true;
                                                     return QuitResult{ true };
                                                 });
    }
}
