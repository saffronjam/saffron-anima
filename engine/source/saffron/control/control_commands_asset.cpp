module;

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

module Saffron.Control;

import Saffron.Core;
import Saffron.Json;
import Saffron.Window;
import Saffron.Rendering;
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
            return AssetTypeDto::Mesh;
        }

        auto assetSlotName(AssetSlotDto slot) -> const char*
        {
            return slot == AssetSlotDto::Albedo ? "albedo" : "mesh";
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
                setSelection(ctx.sceneEdit, Entity{ entt::null });
                return projectDto(project);
            });

        registerCommand<PathParams, ProjectInfoDto>(
            reg, "open-project", "open-project {path}",
            [](EngineContext& ctx, const PathParams& params) -> Result<ProjectInfoDto>
            {
                if (params.path.empty())
                {
                    return Err(std::string{ "missing 'path'" });
                }
                ProjectInfo project;
                auto result = loadProject(ctx.assets, ctx.renderer, ctx.sceneEdit.registry, ctx.sceneEdit.scene,
                                          project, params.path);
                if (!result)
                {
                    return Err(result.error());
                }
                applyProjectInfo(ctx, project);
                ctx.sceneEdit.sceneVersion += 1;
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
                Entity entity = spawnModel(ctx.sceneEdit.scene, "Mesh", *imported);
                ctx.sceneEdit.sceneVersion += 1;
                setSelection(ctx.sceneEdit, entity);
                EntityRef ref = entityRefDto(ctx.sceneEdit.scene, entity);
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

        registerCommand<EmptyParams, AssetList>(
            reg, "list-assets", "list the project asset catalog",
            [](EngineContext& ctx, const EmptyParams&) -> Result<AssetList>
            {
                AssetList out;
                for (const AssetEntry& entry : ctx.assets.catalog.entries)
                {
                    out.assets.push_back(
                        AssetEntryDto{ WireUuid{ entry.id.value }, entry.name, assetTypeDto(entry.type), entry.path });
                }
                return out;
            });

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
                        return AssetRef{ WireUuid{ entry.id.value }, entry.name };
                    }
                }
                return Err(std::format("no asset '{}'", selector));
            });

        registerCommand<AssignAssetParams, AssignAssetResult>(
            reg, "assign-asset", "assign-asset {entity, slot:mesh|albedo, id|name}",
            [](EngineContext& ctx, const AssignAssetParams& params) -> Result<AssignAssetResult>
            {
                auto entity = resolveEntity(ctx, json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                auto resolved = resolveAsset(ctx, params.asset);
                if (!resolved)
                {
                    return Err(resolved.error());
                }
                const AssetEntry* match = *resolved;
                if (params.slot == AssetSlotDto::Mesh)
                {
                    if (!hasComponent<MeshComponent>(ctx.sceneEdit.scene, *entity))
                    {
                        addComponent<MeshComponent>(ctx.sceneEdit.scene, *entity);
                    }
                    getComponent<MeshComponent>(ctx.sceneEdit.scene, *entity).mesh = match->id;
                }
                else if (params.slot == AssetSlotDto::Albedo)
                {
                    if (!hasComponent<MaterialComponent>(ctx.sceneEdit.scene, *entity))
                    {
                        addComponent<MaterialComponent>(ctx.sceneEdit.scene, *entity);
                    }
                    getComponent<MaterialComponent>(ctx.sceneEdit.scene, *entity).albedoTexture = match->id;
                }
                ctx.sceneEdit.sceneVersion += 1;
                return AssignAssetResult{ WireUuid{ match->id.value }, match->name, params.slot };
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
                auto result = saveProject(ctx.assets, ctx.sceneEdit.registry, ctx.sceneEdit.scene, project, path);
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
                const std::string path = params.path.value_or("project.json");
                ProjectInfo project;
                Result<void> result =
                    loadProject(ctx.assets, ctx.renderer, ctx.sceneEdit.registry, ctx.sceneEdit.scene, project, path);
                if (!result)
                {
                    return Err(result.error());
                }
                applyProjectInfo(ctx, project);
                ctx.sceneEdit.sceneVersion += 1;
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
