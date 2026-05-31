module;

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

#include <cstdlib>
#include <format>
#include <string>

module Saffron.Control;

import Saffron.Core;
import Saffron.Window;
import Saffron.Rendering;
import Saffron.Scene;
import Saffron.Editor;
import Saffron.Assets;

namespace se
{
    void registerAssetCommands(CommandRegistry& reg)
    {
        // Imports + bakes a model, then spawns an entity carrying it (selected).
        registerCommand(reg, "import-model", "import-model {path}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const std::string path = asString(positionalOr(params, "path", 0), "");
                if (path.empty())
                {
                    return Err(std::string{ "missing 'path'" });
                }
                auto imported = importModel(ctx.assets, ctx.renderer, path);
                if (!imported)
                {
                    return Err(imported.error());
                }
                Entity entity = spawnModel(ctx.editor.scene, "Mesh", *imported);
                setSelection(ctx.editor, entity);
                json result = entityRef(ctx.editor.scene, entity);
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
                    out.push_back(json{ { "id", entry.id.value }, { "name", entry.name },
                                        { "type", assetTypeName(entry.type) }, { "path", entry.path } });
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
                    if (!hasComponent<MeshComponent>(ctx.editor.scene, *entity))
                    {
                        addComponent<MeshComponent>(ctx.editor.scene, *entity);
                    }
                    getComponent<MeshComponent>(ctx.editor.scene, *entity).mesh = match->id;
                }
                else if (slot == "albedo")
                {
                    if (!hasComponent<MaterialComponent>(ctx.editor.scene, *entity))
                    {
                        addComponent<MaterialComponent>(ctx.editor.scene, *entity);
                    }
                    getComponent<MaterialComponent>(ctx.editor.scene, *entity).albedoTexture = match->id;
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
                auto result = writeScene(ctx.editor.registry, ctx.editor.scene, path);
                if (!result)
                {
                    return Err(result.error());
                }
                ctx.editor.scenePath = path;
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
                auto result = readScene(ctx.editor.registry, ctx.editor.scene, path);
                if (!result)
                {
                    return Err(result.error());
                }
                ctx.editor.scenePath = path;
                setSelection(ctx.editor, Entity{ entt::null });
                return json{ { "path", path } };
            });

        registerCommand(reg, "save-project", "save-project {path} — assets catalog + scene in one file",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const std::string path = asString(positionalOr(params, "path", 0), "project.json");
                auto result = saveProject(ctx.assets, ctx.editor.registry, ctx.editor.scene, path);
                if (!result)
                {
                    return Err(result.error());
                }
                ctx.editor.scenePath = path;
                return json{ { "path", path } };
            });

        registerCommand(reg, "load-project", "load-project {path} — assets catalog + scene",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const std::string path = asString(positionalOr(params, "path", 0), "project.json");
                Result<void> result =
                    loadProject(ctx.assets, ctx.renderer, ctx.editor.registry, ctx.editor.scene, path);
                if (!result)
                {
                    return Err(result.error());
                }
                ctx.editor.scenePath = path;
                setSelection(ctx.editor, Entity{ entt::null });
                return json{ { "path", path } };
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

        registerCommand(reg, "quit", "close the running app",
            [](EngineContext& ctx, const json&) -> Result<json>
            {
                ctx.window.shouldClose = true;
                return json{ { "quitting", true } };
            });
    }
}
