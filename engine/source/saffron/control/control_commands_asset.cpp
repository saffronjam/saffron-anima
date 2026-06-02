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

        auto projectResult(const ProjectInfo& project) -> json
        {
            return projectInfoJson(project);
        }

        auto requireProjectLoaded(EngineContext& ctx) -> Result<void>
        {
            if (!ctx.sceneEdit.projectLoaded)
            {
                return Err(std::string{ "no project loaded" });
            }
            return {};
        }

        // Resolves {asset:id|name, size?} to a base64 PNG preview (mesh = framed 3D render,
        // texture = the image read back). Shared by get-thumbnail (128) + view-asset (512).
        auto thumbnailResult(EngineContext& ctx, const json& params, u32 defaultSize) -> Result<json>
        {
            // The selector may arrive as a name (string), a numeric-string uuid, or — when
            // the CLI coerces a bare numeric arg — a JSON number; accept all three.
            const json sel = positionalOr(params, "asset", 0);
            std::string selector = sel.is_string() ? sel.get<std::string>() : std::string{};
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
            const AssetEntry* match = nullptr;
            for (const AssetEntry& entry : ctx.assets.catalog.entries)
            {
                if (entry.id.value == byId || entry.name == selector)
                {
                    match = &entry;
                }
            }
            if (match == nullptr)
            {
                return Err(std::format("no asset '{}'", selector));
            }
            u32 size = defaultSize;
            const json sz = positionalOr(params, "size", 1);
            if (sz.is_number())
            {
                size = static_cast<u32>(sz.get<double>());
            }

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
            return json{ { "id", match->id.value },
                         { "format", "png" },
                         { "width", size },
                         { "height", size },
                         { "base64", base64Encode(png) } };
        }
    }

    void registerAssetCommands(CommandRegistry& reg)
    {
        registerCommand(reg, "get-project", "get-project — active project metadata",
                        [](EngineContext& ctx, const json&) -> Result<json>
                        { return projectResult(currentProjectInfo(ctx)); });

        registerCommand(reg, "new-project", "new-project {name, displayName?, root?}",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        {
                            const std::string name = asString(positionalOr(params, "name", 0), "");
                            const std::string displayName = asString(positionalOr(params, "displayName", 1), "");
                            const std::string root = asString(positionalOr(params, "root", 2), "");
                            ProjectInfo project;
                            auto result = createProject(ctx.assets, ctx.renderer, ctx.sceneEdit.registry,
                                                        ctx.sceneEdit.scene, project, name, displayName, root);
                            if (!result)
                            {
                                return Err(result.error());
                            }
                            applyProjectInfo(ctx, project);
                            ctx.sceneEdit.sceneVersion += 1;
                            setSelection(ctx.sceneEdit, Entity{ entt::null });
                            return projectResult(project);
                        });

        registerCommand(reg, "open-project", "open-project {path}",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        {
                            const std::string path = asString(positionalOr(params, "path", 0), "");
                            if (path.empty())
                            {
                                return Err(std::string{ "missing 'path'" });
                            }
                            ProjectInfo project;
                            auto result = loadProject(ctx.assets, ctx.renderer, ctx.sceneEdit.registry,
                                                      ctx.sceneEdit.scene, project, path);
                            if (!result)
                            {
                                return Err(result.error());
                            }
                            applyProjectInfo(ctx, project);
                            ctx.sceneEdit.sceneVersion += 1;
                            setSelection(ctx.sceneEdit, Entity{ entt::null });
                            return projectResult(project);
                        });

        // Imports + bakes a model, then spawns an entity carrying it (selected).
        registerCommand(reg, "import-model", "import-model {path}",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        {
                            const std::string path = asString(positionalOr(params, "path", 0), "");
                            if (path.empty())
                            {
                                return Err(std::string{ "missing 'path'" });
                            }
                            if (auto ready = requireProjectLoaded(ctx); !ready)
                            {
                                return Err(ready.error());
                            }
                            auto imported = importModel(ctx.assets, ctx.renderer, path);
                            if (!imported)
                            {
                                return Err(imported.error());
                            }
                            Entity entity = spawnModel(ctx.sceneEdit.scene, "Mesh", *imported);
                            setSelection(ctx.sceneEdit, entity);
                            json result = entityRef(ctx.sceneEdit.scene, entity);
                            result["mesh"] = imported->mesh.value;
                            result["albedoTexture"] = imported->albedoTexture.value;
                            return result;
                        });

        // Imports an external image into the asset dir; returns its texture id (assign
        // it with set-material --albedoTexture <id>).
        registerCommand(reg, "import-texture", "import-texture {path}",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        {
                            const std::string path = asString(positionalOr(params, "path", 0), "");
                            if (path.empty())
                            {
                                return Err(std::string{ "missing 'path'" });
                            }
                            if (auto ready = requireProjectLoaded(ctx); !ready)
                            {
                                return Err(ready.error());
                            }
                            auto id = importTexture(ctx.assets, ctx.renderer, path);
                            if (!id)
                            {
                                return Err(id.error());
                            }
                            return json{ { "texture", id->value } };
                        });

        registerCommand(reg, "list-assets", "list the project asset catalog",
                        [](EngineContext& ctx, const json&) -> Result<json>
                        {
                            json out = json::array();
                            for (const AssetEntry& entry : ctx.assets.catalog.entries)
                            {
                                out.push_back(json{ { "id", entry.id.value },
                                                    { "name", entry.name },
                                                    { "type", assetTypeName(entry.type) },
                                                    { "path", entry.path } });
                            }
                            return json{ { "assets", std::move(out) } };
                        });

        registerCommand(reg, "rename-asset", "rename-asset {id|name, newName}",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        {
                            const std::string selector = asString(positionalOr(params, "asset", 0), "");
                            const std::string newName = asString(positionalOr(params, "name", 1), "");
                            if (selector.empty() || newName.empty())
                            {
                                return Err(std::string{ "usage: rename-asset {id|name} {newName}" });
                            }
                            const u64 byId = std::strtoull(selector.c_str(), nullptr, 10);
                            for (AssetEntry& entry : ctx.assets.catalog.entries)
                            {
                                if (entry.id.value == byId || entry.name == selector)
                                {
                                    entry.name = newName;
                                    return json{ { "id", entry.id.value }, { "name", entry.name } };
                                }
                            }
                            return Err(std::format("no asset '{}'", selector));
                        });

        registerCommand(reg, "assign-asset", "assign-asset {entity, slot:mesh|albedo, id|name}",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        {
                            auto entity = resolveEntity(ctx, params);
                            if (!entity)
                            {
                                return Err(entity.error());
                            }
                            const std::string slot = asString(positionalOr(params, "slot", 1), "");
                            const std::string selector = asString(positionalOr(params, "asset", 2), "");
                            const u64 byId = std::strtoull(selector.c_str(), nullptr, 10);
                            const AssetEntry* match = nullptr;
                            for (const AssetEntry& entry : ctx.assets.catalog.entries)
                            {
                                if (entry.id.value == byId || entry.name == selector)
                                {
                                    match = &entry;
                                }
                            }
                            if (match == nullptr)
                            {
                                return Err(std::format("no asset '{}'", selector));
                            }
                            if (slot == "mesh")
                            {
                                if (!hasComponent<MeshComponent>(ctx.sceneEdit.scene, *entity))
                                {
                                    addComponent<MeshComponent>(ctx.sceneEdit.scene, *entity);
                                }
                                getComponent<MeshComponent>(ctx.sceneEdit.scene, *entity).mesh = match->id;
                            }
                            else if (slot == "albedo")
                            {
                                if (!hasComponent<MaterialComponent>(ctx.sceneEdit.scene, *entity))
                                {
                                    addComponent<MaterialComponent>(ctx.sceneEdit.scene, *entity);
                                }
                                getComponent<MaterialComponent>(ctx.sceneEdit.scene, *entity).albedoTexture = match->id;
                            }
                            else
                            {
                                return Err(std::string{ "slot must be 'mesh' or 'albedo'" });
                            }
                            return json{ { "id", match->id.value }, { "name", match->name }, { "slot", slot } };
                        });

        registerCommand(reg, "save-scene", "save-scene {path}",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        {
                            const std::string path = asString(positionalOr(params, "path", 0), "");
                            if (path.empty())
                            {
                                return Err(std::string{ "missing 'path'" });
                            }
                            auto result = writeScene(ctx.sceneEdit.registry, ctx.sceneEdit.scene, path);
                            if (!result)
                            {
                                return Err(result.error());
                            }
                            ctx.sceneEdit.scenePath = path;
                            return json{ { "path", path } };
                        });

        registerCommand(reg, "load-scene", "load-scene {path}",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        {
                            const std::string path = asString(positionalOr(params, "path", 0), "");
                            if (path.empty())
                            {
                                return Err(std::string{ "missing 'path'" });
                            }
                            auto result = readScene(ctx.sceneEdit.registry, ctx.sceneEdit.scene, path);
                            if (!result)
                            {
                                return Err(result.error());
                            }
                            ctx.sceneEdit.scenePath = path;
                            ctx.sceneEdit.sceneVersion += 1;
                            setSelection(ctx.sceneEdit, Entity{ entt::null });
                            return json{ { "path", path } };
                        });

        registerCommand(reg, "save-project", "save-project {path} — assets catalog + scene in one file",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        {
                            std::string path = asString(positionalOr(params, "path", 0), "");
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
                            auto result =
                                saveProject(ctx.assets, ctx.sceneEdit.registry, ctx.sceneEdit.scene, project, path);
                            if (!result)
                            {
                                return Err(result.error());
                            }
                            project.path = path;
                            applyProjectInfo(ctx, project);
                            return projectResult(project);
                        });

        registerCommand(reg, "load-project", "load-project {path} — assets catalog + scene",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        {
                            const std::string path = asString(positionalOr(params, "path", 0), "project.json");
                            ProjectInfo project;
                            Result<void> result = loadProject(ctx.assets, ctx.renderer, ctx.sceneEdit.registry,
                                                              ctx.sceneEdit.scene, project, path);
                            if (!result)
                            {
                                return Err(result.error());
                            }
                            applyProjectInfo(ctx, project);
                            ctx.sceneEdit.sceneVersion += 1;
                            setSelection(ctx.sceneEdit, Entity{ entt::null });
                            return projectResult(project);
                        });

        registerCommand(reg, "screenshot", "screenshot {target:viewport|window, path}",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        {
                            const std::string target = asString(positionalOr(params, "target", 0), "viewport");
                            const std::string path = asString(positionalOr(params, "path", 1), "");
                            if (path.empty())
                            {
                                return Err(std::string{ "missing 'path'" });
                            }
                            if (target == "viewport")
                            {
                                auto shot = captureViewport(ctx.renderer, path);
                                if (!shot)
                                {
                                    return Err(shot.error());
                                }
                                return json{ { "target", target }, { "path", path }, { "pending", false } };
                            }
                            if (target == "window")
                            {
                                // Written at the end of the current frame.
                                auto shot = requestWindowCapture(ctx.renderer, path);
                                if (!shot)
                                {
                                    return Err(shot.error());
                                }
                                return json{ { "target", target }, { "path", path }, { "pending", true } };
                            }
                            return Err(std::format("unknown target '{}' (viewport|window)", target));
                        });

        registerCommand(reg, "get-thumbnail", "get-thumbnail {asset:id|name, size=128} — base64 PNG preview",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        { return thumbnailResult(ctx, params, 128); });

        registerCommand(reg, "view-asset", "view-asset {asset:id|name, size=512} — larger base64 PNG preview",
                        [](EngineContext& ctx, const json& params) -> Result<json>
                        { return thumbnailResult(ctx, params, 512); });

        registerCommand(reg, "quit", "close the running app",
                        [](EngineContext& ctx, const json&) -> Result<json>
                        {
                            ctx.window.shouldClose = true;
                            return json{ { "quitting", true } };
                        });
    }
}
