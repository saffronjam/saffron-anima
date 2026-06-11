module;

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

#include <chrono>
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
            return AssetEntryDto{ WireUuid{ entry.id.value }, entry.name, assetTypeDto(entry.type), entry.path,
                                  entry.folder.empty() ? std::optional<std::string>{}
                                                       : std::optional<std::string>{ entry.folder } };
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

        // Resolves {asset:id|name, size?} to a base64 PNG preview (mesh = framed 3D render,
        // texture = the image read back). Shared by get-thumbnail (128) + view-asset (512).
        auto thumbnailResult(EngineContext& ctx, const ThumbnailParams& params, u32 defaultSize)
            -> Result<ThumbnailResult>
        {
            auto resolved = resolveAsset(ctx, params.asset);
            if (!resolved)
            {
                return Err(resolved.error());
            }
            const AssetEntry* match = *resolved;
            const u32 size = static_cast<u32>(params.size.value_or(static_cast<i32>(defaultSize)));

            std::vector<u8> png;
            if (match->type == AssetType::Mesh)
            {
                auto mesh = loadMeshAsset(ctx.assets, ctx.renderer, match->id);
                if (!mesh)
                {
                    return Err(std::string{ "mesh failed to load" });
                }
                auto bytes = encodeAssetThumbnailPng(ctx.renderer, mesh, size);
                if (!bytes)
                {
                    return Err(bytes.error());
                }
                png = std::move(*bytes);
            }
            else if (match->type == AssetType::Texture)
            {
                auto tex = loadTextureAsset(ctx.assets, ctx.renderer, match->id);
                if (!tex)
                {
                    return Err(std::string{ "texture failed to load" });
                }
                auto bytes = encodeTextureThumbnailPng(ctx.renderer, tex, size);
                if (!bytes)
                {
                    return Err(bytes.error());
                }
                png = std::move(*bytes);
            }
            else if (match->type == AssetType::Material)
            {
                auto loaded = loadMaterialAsset(ctx.assets, match->id);
                if (!loaded)
                {
                    return Err(loaded.error());
                }
                const SubmeshMaterial sm = resolveMaterialAsset(ctx.assets, ctx.renderer, *loaded);
                auto tex = renderMaterialPreview(ctx.renderer, sm, size);
                if (!tex)
                {
                    return Err(tex.error());
                }
                auto bytes = encodeTextureThumbnailPng(ctx.renderer, *tex, size);
                if (!bytes)
                {
                    return Err(bytes.error());
                }
                png = std::move(*bytes);
            }
            else
            {
                return Err(std::string{ "asset has no thumbnail" });
            }
            return ThumbnailResult{ WireUuid{ match->id.value }, "png", static_cast<i32>(size), static_cast<i32>(size),
                                    base64Encode(png) };
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

        // Imports + bakes a model, then spawns an entity carrying it (selected).
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
                auto imported = importModel(ctx.assets, ctx.renderer, params.path);
                if (!imported)
                {
                    return Err(imported.error());
                }
                Entity entity = spawnModel(activeScene(ctx.sceneEdit), "Mesh", *imported);
                ctx.sceneEdit.sceneVersion += 1;
                setSelection(ctx.sceneEdit, entity);
                EntityRef ref = entityRefDto(activeScene(ctx.sceneEdit), entity);
                return ImportModelResult{ ref.id, ref.name, WireUuid{ imported->mesh.value },
                                          WireUuid{ imported->albedoTexture.value } };
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
                ctx.sceneEdit.sceneVersion += 1;
                return DeleteAssetResult{ WireUuid{ entry.id.value }, entry.name, std::move(cleared), fileDeleted };
            });

        registerCommand<AssignAssetParams, AssignAssetResult>(
            reg, "assign-asset", "assign-asset {entity, slot:mesh|albedo|metallic-roughness, id|name}",
            [](EngineContext& ctx, const AssignAssetParams& params) -> Result<AssignAssetResult>
            {
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
                return PreviewRenderResult{ base64Encode(*png) };
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

        registerCommand<EmptyParams, QuitResult>(reg, "quit", "close the running app",
                                                 [](EngineContext& ctx, const EmptyParams&) -> Result<QuitResult>
                                                 {
                                                     ctx.window.shouldClose = true;
                                                     return QuitResult{ true };
                                                 });
    }
}
