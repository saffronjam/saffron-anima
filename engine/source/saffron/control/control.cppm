module;

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>

#include <expected>
#include <format>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

export module Saffron.Control;

import Saffron.Core;
import Saffron.Window;
import Saffron.Rendering;
import Saffron.Scene;
import Saffron.Editor;
import Saffron.Assets;

export namespace se
{
    using json = nlohmann::json;

    /// The slice of live engine state a command may touch. References only; built
    /// fresh each frame and never stored past it.
    struct EngineContext
    {
        Window& window;
        Renderer& renderer;
        EditorContext& editor;
        AssetServer& assets;
    };

    /// A control command: a name, one-line help, and a handler that runs on the
    /// main thread and returns its result json or an error message.
    struct CommandTraits
    {
        std::string name;
        std::string help;
        std::function<std::expected<json, std::string>(EngineContext&, const json&)> run;
    };

    struct CommandRegistry
    {
        std::vector<CommandTraits> rows;
        std::unordered_map<std::string, std::size_t> byName;
    };

    void registerCommand(CommandRegistry& reg, std::string name, std::string help,
                         std::function<std::expected<json, std::string>(EngineContext&, const json&)> run)
    {
        const std::size_t index = reg.rows.size();
        reg.byName[name] = index;
        reg.rows.push_back(CommandTraits{ std::move(name), std::move(help), std::move(run) });
    }

    const CommandTraits* findCommand(const CommandRegistry& reg, const std::string& name)
    {
        auto it = reg.byName.find(name);
        if (it == reg.byName.end())
        {
            return nullptr;
        }
        return &reg.rows[it->second];
    }

    // params[name] if present, else the index-th element of params["args"], else null.
    // Lets every command accept either `--name value` or a bare positional.
    json positionalOr(const json& params, const std::string& name, std::size_t index)
    {
        if (params.contains(name))
        {
            return params[name];
        }
        if (params.contains("args") && params["args"].is_array() && index < params["args"].size())
        {
            return params["args"][index];
        }
        return json{};
    }

    std::string asString(const json& value, std::string fallback)
    {
        if (value.is_string())
        {
            return value.get<std::string>();
        }
        return fallback;
    }

    std::expected<Entity, std::string> resolveEntity(EngineContext& ctx, const json& params)
    {
        const json selector = positionalOr(params, "entity", 0);
        if (selector.is_null())
        {
            return std::unexpected(std::string{ "missing 'entity' (uuid or name)" });
        }
        Scene& scene = ctx.editor.scene;

        // UUID first (stable across reloads); a numeric string counts as a UUID.
        u64 wanted = 0;
        bool haveUuid = false;
        if (selector.is_number_unsigned())
        {
            wanted = selector.get<u64>();
            haveUuid = true;
        }
        else if (selector.is_string())
        {
            const std::string text = selector.get<std::string>();
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
            if (end != text.c_str() && *end == '\0')
            {
                wanted = parsed;
                haveUuid = true;
            }
        }

        Entity found{ entt::null };
        if (haveUuid)
        {
            forEach<IdComponent>(scene, [&](Entity entity, IdComponent& id)
            {
                if (id.id.value == wanted)
                {
                    found = entity;
                }
            });
            if (found.handle != entt::null)
            {
                return found;
            }
        }
        if (selector.is_string())
        {
            const std::string name = selector.get<std::string>();
            forEach<NameComponent>(scene, [&](Entity entity, NameComponent& component)
            {
                if (found.handle == entt::null && component.name == name)
                {
                    found = entity;
                }
            });
        }
        if (found.handle == entt::null)
        {
            return std::unexpected(std::format("entity not found: {}", selector.dump()));
        }
        return found;
    }

    json entityRef(Scene& scene, Entity entity)
    {
        return json{ { "id", getComponent<IdComponent>(scene, entity).id.value },
                     { "name", getComponent<NameComponent>(scene, entity).name } };
    }

    void registerBuiltinCommands(CommandRegistry& reg)
    {
        registerCommand(reg, "ping", "liveness + engine info",
            [](EngineContext&, const json&) -> std::expected<json, std::string>
            {
                return json{ { "pong", true },
                             { "engine", std::string{ EngineName } },
                             { "version", std::string{ EngineVersion } },
                             { "pid", static_cast<int>(::getpid()) } };
            });

        registerCommand(reg, "help", "list available commands",
            [&reg](EngineContext&, const json&) -> std::expected<json, std::string>
            {
                json commands = json::array();
                for (const CommandTraits& command : reg.rows)
                {
                    commands.push_back(json{ { "name", command.name }, { "help", command.help } });
                }
                return json{ { "commands", std::move(commands) } };
            });

        registerCommand(reg, "render-stats", "last frame's scene draw counters",
            [](EngineContext& ctx, const json&) -> std::expected<json, std::string>
            {
                const RenderStats stats = renderStats(ctx.renderer);
                return json{ { "drawCalls", stats.drawCalls },
                             { "batches", stats.batches },
                             { "instances", stats.instances },
                             { "clustered", clusteredEnabled(ctx.renderer) },
                             { "postProcess", postProcessEnabled(ctx.renderer) },
                             { "depthPrepass", depthPrepassEnabled(ctx.renderer) },
                             { "pipelines", pipelineCount(ctx.renderer) },
                             { "aa", aaMode(ctx.renderer) } };
            });

        registerCommand(reg, "set-aa", "set-aa {off|fxaa|msaa2|msaa4|msaa8} — anti-aliasing mode",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const json value = positionalOr(params, "mode", 0);
                std::string mode = "off";
                if (value.is_string())
                {
                    mode = value.get<std::string>();
                }
                u32 samples = 1;
                bool fxaa = false;
                if (mode == "fxaa") { fxaa = true; }
                else if (mode == "msaa2") { samples = 2; }
                else if (mode == "msaa4") { samples = 4; }
                else if (mode == "msaa8") { samples = 8; }
                else if (mode != "off")
                {
                    return std::unexpected(std::string{ "expected off|fxaa|msaa2|msaa4|msaa8" });
                }
                setAa(ctx.renderer, samples, fxaa);
                return json{ { "aa", aaMode(ctx.renderer) } };
            });

        registerCommand(reg, "set-clustered", "set-clustered {0|1} — toggle clustered light culling",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number())
                {
                    enabled = value.get<double>() != 0.0;
                }
                else if (value.is_boolean())
                {
                    enabled = value.get<bool>();
                }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setClustered(ctx.renderer, enabled);
                return json{ { "clustered", enabled } };
            });

        registerCommand(reg, "set-postprocess", "set-postprocess {0|1} — toggle the post-process tonemap pass",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number())
                {
                    enabled = value.get<double>() != 0.0;
                }
                else if (value.is_boolean())
                {
                    enabled = value.get<bool>();
                }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setPostProcess(ctx.renderer, enabled);
                return json{ { "postProcess", enabled } };
            });

        registerCommand(reg, "set-depth-prepass", "set-depth-prepass {0|1} — toggle the depth pre-pass",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number())
                {
                    enabled = value.get<double>() != 0.0;
                }
                else if (value.is_boolean())
                {
                    enabled = value.get<bool>();
                }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setDepthPrepass(ctx.renderer, enabled);
                return json{ { "depthPrepass", enabled } };
            });

        registerCommand(reg, "list-entities", "list all entities",
            [](EngineContext& ctx, const json&) -> std::expected<json, std::string>
            {
                json entities = json::array();
                forEach<IdComponent, NameComponent>(ctx.editor.scene,
                    [&](Entity, IdComponent& id, NameComponent& name)
                    {
                        entities.push_back(json{ { "id", id.id.value }, { "name", name.name } });
                    });
                return json{ { "entities", std::move(entities) } };
            });

        registerCommand(reg, "list-components", "list registered component types",
            [](EngineContext& ctx, const json&) -> std::expected<json, std::string>
            {
                json names = json::array();
                for (const ComponentTraits& traits : ctx.editor.registry.rows)
                {
                    names.push_back(traits.name);
                }
                return json{ { "components", std::move(names) } };
            });

        registerCommand(reg, "create-entity", "create-entity {name}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const std::string name = asString(positionalOr(params, "name", 0), "Entity");
                Entity entity = createEntity(ctx.editor.scene, name);
                return entityRef(ctx.editor.scene, entity);
            });

        // Imports + bakes a model, then spawns an entity carrying it (selected).
        registerCommand(reg, "import-model", "import-model {path}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const std::string path = asString(positionalOr(params, "path", 0), "");
                if (path.empty())
                {
                    return std::unexpected(std::string{ "missing 'path'" });
                }
                std::expected<ImportResult, std::string> imported = importModel(ctx.assets, ctx.renderer, path);
                if (!imported)
                {
                    return std::unexpected(imported.error());
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
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const std::string path = asString(positionalOr(params, "path", 0), "");
                if (path.empty())
                {
                    return std::unexpected(std::string{ "missing 'path'" });
                }
                std::expected<Uuid, std::string> id = importTexture(ctx.assets, ctx.renderer, path);
                if (!id)
                {
                    return std::unexpected(id.error());
                }
                return json{ { "texture", id->value } };
            });

        registerCommand(reg, "destroy-entity", "destroy-entity {entity}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                std::expected<Entity, std::string> entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return std::unexpected(entity.error());
                }
                const u64 id = getComponent<IdComponent>(ctx.editor.scene, *entity).id.value;
                if (ctx.editor.selected.handle == entity->handle)
                {
                    setSelection(ctx.editor, Entity{ entt::null });
                }
                destroyEntity(ctx.editor.scene, *entity);
                return json{ { "destroyed", id } };
            });

        registerCommand(reg, "add-component", "add-component {entity, component}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                std::expected<Entity, std::string> entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return std::unexpected(entity.error());
                }
                const std::string name = asString(positionalOr(params, "component", 1), "");
                const ComponentTraits* row = findByName(ctx.editor.registry, name);
                if (row == nullptr)
                {
                    return std::unexpected(std::format("unknown component '{}'", name));
                }
                if (row->has(ctx.editor.scene, *entity))
                {
                    return std::unexpected(std::format("entity already has '{}'", name));
                }
                row->addDefault(ctx.editor.scene, *entity);
                return json{ { "added", row->name } };
            });

        registerCommand(reg, "remove-component", "remove-component {entity, component}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                std::expected<Entity, std::string> entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return std::unexpected(entity.error());
                }
                const std::string name = asString(positionalOr(params, "component", 1), "");
                const ComponentTraits* row = findByName(ctx.editor.registry, name);
                if (row == nullptr)
                {
                    return std::unexpected(std::format("unknown component '{}'", name));
                }
                if (!row->removable)
                {
                    return std::unexpected(std::format("component '{}' is not removable", row->name));
                }
                row->remove(ctx.editor.scene, *entity);
                return json{ { "removed", row->name } };
            });

        // Applies a component's serialized form. Routing through the registry's
        // deserialize keeps the wire shape identical to scene files.
        registerCommand(reg, "set-component", "set-component {entity, component, json}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                std::expected<Entity, std::string> entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return std::unexpected(entity.error());
                }
                const std::string name = asString(positionalOr(params, "component", 1), "");
                const ComponentTraits* row = findByName(ctx.editor.registry, name);
                if (row == nullptr)
                {
                    return std::unexpected(std::format("unknown component '{}'", name));
                }
                const json body = params.value("json", json::object());
                std::expected<void, std::string> result = row->deserialize(ctx.editor.scene, *entity, body);
                if (!result)
                {
                    return std::unexpected(result.error());
                }
                return json{ { "set", row->name } };
            });

        // Routes through the Transform row's deserialize so the wire shape matches
        // scene files exactly: {translation:{x,y,z}, rotation:{x,y,z} Euler radians, scale:{x,y,z}}.
        registerCommand(reg, "set-transform", "set-transform {entity, translation?, rotation?, scale?}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                std::expected<Entity, std::string> entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return std::unexpected(entity.error());
                }
                const ComponentTraits* row = findByName(ctx.editor.registry, "Transform");
                if (row == nullptr)
                {
                    return std::unexpected(std::string{ "Transform component is not registered" });
                }
                if (!row->has(ctx.editor.scene, *entity))
                {
                    return std::unexpected(std::string{ "entity has no Transform" });
                }
                // Merge provided fields over the current transform so unspecified
                // fields (e.g. scale) are preserved rather than reset to defaults.
                json body = row->serialize(ctx.editor.scene, *entity);
                if (params.contains("translation")) { body["translation"] = params["translation"]; }
                if (params.contains("rotation")) { body["rotation"] = params["rotation"]; }
                if (params.contains("scale")) { body["scale"] = params["scale"]; }
                std::expected<void, std::string> result = row->deserialize(ctx.editor.scene, *entity, body);
                if (!result)
                {
                    return std::unexpected(result.error());
                }
                return entityRef(ctx.editor.scene, *entity);
            });

        // Adds/updates the entity's Material, merging the provided fields over its
        // current value (baseColor as {x,y,z,w}).
        registerCommand(reg, "set-material", "set-material {entity, baseColor?:{x,y,z,w}, albedoTexture?:uuid, unlit?:0|1}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                std::expected<Entity, std::string> entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return std::unexpected(entity.error());
                }
                const ComponentTraits* row = findByName(ctx.editor.registry, "Material");
                if (row == nullptr)
                {
                    return std::unexpected(std::string{ "Material component is not registered" });
                }
                if (!row->has(ctx.editor.scene, *entity))
                {
                    row->addDefault(ctx.editor.scene, *entity);
                }
                json body = row->serialize(ctx.editor.scene, *entity);
                if (params.contains("baseColor")) { body["baseColor"] = params["baseColor"]; }
                if (params.contains("albedoTexture"))
                {
                    // The se CLI passes a bare uuid as a string; coerce it to a number so
                    // the component's value<u64> deserialize doesn't abort (JSON_NOEXCEPTION).
                    const json& a = params["albedoTexture"];
                    if (a.is_string())
                    {
                        const std::string s = a.get<std::string>();
                        u64 id = 0;
                        std::from_chars(s.data(), s.data() + s.size(), id);
                        body["albedoTexture"] = id;
                    }
                    else
                    {
                        body["albedoTexture"] = a;
                    }
                }
                if (params.contains("unlit"))
                {
                    const json& u = params["unlit"];
                    bool unlit = false;
                    if (u.is_number())
                    {
                        unlit = u.get<double>() != 0.0;
                    }
                    else if (u.is_boolean())
                    {
                        unlit = u.get<bool>();
                    }
                    else if (u.is_string())
                    {
                        const std::string s = u.get<std::string>();
                        unlit = !(s == "0" || s == "false" || s == "off");
                    }
                    body["unlit"] = unlit;
                }
                std::expected<void, std::string> result = row->deserialize(ctx.editor.scene, *entity, body);
                if (!result)
                {
                    return std::unexpected(result.error());
                }
                return entityRef(ctx.editor.scene, *entity);
            });

        // Sets the directional light (the given entity, else the first one),
        // merging provided fields (direction/color as {x,y,z}) over its current value.
        registerCommand(reg, "set-light", "set-light {entity?, direction?, color?, intensity?, ambient?}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const ComponentTraits* row = findByName(ctx.editor.registry, "DirectionalLight");
                if (row == nullptr)
                {
                    return std::unexpected(std::string{ "DirectionalLight component is not registered" });
                }
                Entity target{ entt::null };
                const json selector = positionalOr(params, "entity", 0);
                if (!selector.is_null())
                {
                    std::expected<Entity, std::string> resolved = resolveEntity(ctx, params);
                    if (!resolved)
                    {
                        return std::unexpected(resolved.error());
                    }
                    target = *resolved;
                }
                else
                {
                    forEach<DirectionalLightComponent>(ctx.editor.scene, [&](Entity entity, DirectionalLightComponent&)
                    {
                        if (target.handle == entt::null)
                        {
                            target = entity;
                        }
                    });
                }
                if (target.handle == entt::null || !row->has(ctx.editor.scene, target))
                {
                    return std::unexpected(std::string{ "no DirectionalLight to set" });
                }
                json body = row->serialize(ctx.editor.scene, target);
                if (params.contains("direction")) { body["direction"] = params["direction"]; }
                if (params.contains("color")) { body["color"] = params["color"]; }
                if (params.contains("intensity")) { body["intensity"] = params["intensity"]; }
                if (params.contains("ambient")) { body["ambient"] = params["ambient"]; }
                std::expected<void, std::string> result = row->deserialize(ctx.editor.scene, target, body);
                if (!result)
                {
                    return std::unexpected(result.error());
                }
                return entityRef(ctx.editor.scene, target);
            });

        registerCommand(reg, "select", "select {entity}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                std::expected<Entity, std::string> entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return std::unexpected(entity.error());
                }
                setSelection(ctx.editor, *entity);
                return entityRef(ctx.editor.scene, *entity);
            });

        registerCommand(reg, "pick", "pick {u=0.5, v=0.5} — ray-pick at viewport UV (0,0 = top-left)",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const json uParam = positionalOr(params, "u", 0);
                const json vParam = positionalOr(params, "v", 1);
                f32 u = 0.5f;
                f32 v = 0.5f;
                if (uParam.is_number()) { u = static_cast<f32>(uParam.get<double>()); }
                if (vParam.is_number()) { v = static_cast<f32>(vParam.get<double>()); }
                const CameraView cam = editorCameraView(ctx.editor.camera);
                const Entity hit = pickEntity(ctx.editor.scene, ctx.assets, ctx.renderer, cam,
                                              glm::vec2{ u * 2.0f - 1.0f, v * 2.0f - 1.0f });
                setSelection(ctx.editor, hit);
                if (hit.handle == entt::null)
                {
                    return json{ { "hit", false } };
                }
                json result = entityRef(ctx.editor.scene, hit);
                result["hit"] = true;
                return result;
            });

        registerCommand(reg, "inspect", "inspect {entity} — dump all its components as json",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                std::expected<Entity, std::string> entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return std::unexpected(entity.error());
                }
                json components = json::object();
                for (const ComponentTraits& row : ctx.editor.registry.rows)
                {
                    if (row.has(ctx.editor.scene, *entity))
                    {
                        components[row.name] = row.serialize(ctx.editor.scene, *entity);
                    }
                }
                json result = entityRef(ctx.editor.scene, *entity);
                result["components"] = std::move(components);
                return result;
            });

        registerCommand(reg, "list-assets", "list the project asset catalog",
            [](EngineContext& ctx, const json&) -> std::expected<json, std::string>
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
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const std::string selector = asString(positionalOr(params, "asset", 0), "");
                const std::string newName = asString(positionalOr(params, "name", 1), "");
                if (selector.empty() || newName.empty())
                {
                    return std::unexpected(std::string{ "usage: rename-asset {id|name} {newName}" });
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
                return std::unexpected(std::format("no asset '{}'", selector));
            });

        registerCommand(reg, "assign-asset", "assign-asset {entity, slot:mesh|albedo, id|name}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                std::expected<Entity, std::string> entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return std::unexpected(entity.error());
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
                    return std::unexpected(std::format("no asset '{}'", selector));
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
                    return std::unexpected(std::string{ "slot must be 'mesh' or 'albedo'" });
                }
                return json{ { "id", match->id.value }, { "name", match->name }, { "slot", slot } };
            });

        registerCommand(reg, "focus", "focus {entity} — aim the editor camera at it",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                std::expected<Entity, std::string> entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return std::unexpected(entity.error());
                }
                if (!hasComponent<TransformComponent>(ctx.editor.scene, *entity))
                {
                    return std::unexpected(std::string{ "entity has no Transform" });
                }
                const glm::vec3 target = getComponent<TransformComponent>(ctx.editor.scene, *entity).translation;
                ctx.editor.camera.position = target - editorCameraForward(ctx.editor.camera) * 5.0f;
                return entityRef(ctx.editor.scene, *entity);
            });

        registerCommand(reg, "save-scene", "save-scene {path}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const std::string path = asString(positionalOr(params, "path", 0), "");
                if (path.empty())
                {
                    return std::unexpected(std::string{ "missing 'path'" });
                }
                std::expected<void, std::string> result = writeScene(ctx.editor.registry, ctx.editor.scene, path);
                if (!result)
                {
                    return std::unexpected(result.error());
                }
                ctx.editor.scenePath = path;
                return json{ { "path", path } };
            });

        registerCommand(reg, "load-scene", "load-scene {path}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const std::string path = asString(positionalOr(params, "path", 0), "");
                if (path.empty())
                {
                    return std::unexpected(std::string{ "missing 'path'" });
                }
                std::expected<void, std::string> result = readScene(ctx.editor.registry, ctx.editor.scene, path);
                if (!result)
                {
                    return std::unexpected(result.error());
                }
                ctx.editor.scenePath = path;
                setSelection(ctx.editor, Entity{ entt::null });
                return json{ { "path", path } };
            });

        registerCommand(reg, "save-project", "save-project {path} — assets catalog + scene in one file",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const std::string path = asString(positionalOr(params, "path", 0), "project.json");
                std::expected<void, std::string> result = saveProject(ctx.assets, ctx.editor.registry, ctx.editor.scene, path);
                if (!result)
                {
                    return std::unexpected(result.error());
                }
                ctx.editor.scenePath = path;
                return json{ { "path", path } };
            });

        registerCommand(reg, "load-project", "load-project {path} — assets catalog + scene",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const std::string path = asString(positionalOr(params, "path", 0), "project.json");
                std::expected<void, std::string> result =
                    loadProject(ctx.assets, ctx.renderer, ctx.editor.registry, ctx.editor.scene, path);
                if (!result)
                {
                    return std::unexpected(result.error());
                }
                ctx.editor.scenePath = path;
                setSelection(ctx.editor, Entity{ entt::null });
                return json{ { "path", path } };
            });

        registerCommand(reg, "screenshot", "screenshot {target:viewport|window, path}",
            [](EngineContext& ctx, const json& params) -> std::expected<json, std::string>
            {
                const std::string target = asString(positionalOr(params, "target", 0), "viewport");
                const std::string path = asString(positionalOr(params, "path", 1), "");
                if (path.empty())
                {
                    return std::unexpected(std::string{ "missing 'path'" });
                }
                if (target == "viewport")
                {
                    std::expected<void, std::string> shot = captureViewport(ctx.renderer, path);
                    if (!shot)
                    {
                        return std::unexpected(shot.error());
                    }
                    return json{ { "target", target }, { "path", path }, { "pending", false } };
                }
                if (target == "window")
                {
                    // Written at the end of the current frame.
                    std::expected<void, std::string> shot = requestWindowCapture(ctx.renderer, path);
                    if (!shot)
                    {
                        return std::unexpected(shot.error());
                    }
                    return json{ { "target", target }, { "path", path }, { "pending", true } };
                }
                return std::unexpected(std::format("unknown target '{}' (viewport|window)", target));
            });

        registerCommand(reg, "quit", "close the running app",
            [](EngineContext& ctx, const json&) -> std::expected<json, std::string>
            {
                ctx.window.shouldClose = true;
                return json{ { "quitting", true } };
            });
    }

    std::string controlSocketPath()
    {
        if (const char* override = std::getenv("SAFFRON_CONTROL_SOCK"))
        {
            return std::string{ override };
        }
        if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"))
        {
            return std::format("{}/saffron-control.sock", runtime);
        }
        return std::format("/tmp/saffron-control-{}.sock", static_cast<unsigned>(::getuid()));
    }

    struct ControlClient
    {
        int fd = -1;
        std::string inbuf;
    };

    struct ControlServer
    {
        int listenFd = -1;
        std::string path;
        std::vector<ControlClient> clients;
    };

    std::expected<ControlServer, std::string> startControlServer(std::string path)
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
        {
            return std::unexpected(std::format("socket: {}", std::strerror(errno)));
        }
        ::unlink(path.c_str());

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path))
        {
            ::close(fd);
            return std::unexpected(std::format("socket path too long: {}", path));
        }
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            return std::unexpected(std::format("bind '{}': {}", path, std::strerror(errno)));
        }
        // Owner-only file permission is the access control (with the 0700 runtime dir).
        ::chmod(path.c_str(), 0600);
        if (::listen(fd, 8) != 0)
        {
            ::close(fd);
            return std::unexpected(std::format("listen: {}", std::strerror(errno)));
        }
        return ControlServer{ fd, std::move(path), {} };
    }

    void stopControlServer(ControlServer& server)
    {
        for (ControlClient& client : server.clients)
        {
            if (client.fd >= 0)
            {
                ::close(client.fd);
            }
        }
        server.clients.clear();
        if (server.listenFd >= 0)
        {
            ::close(server.listenFd);
            server.listenFd = -1;
        }
        if (!server.path.empty())
        {
            ::unlink(server.path.c_str());
        }
    }

    json dispatch(CommandRegistry& reg, EngineContext& ctx, const json& request)
    {
        json reply;
        reply["id"] = request.value("id", json{});
        const std::string command = request.value("cmd", std::string{});
        const CommandTraits* row = findCommand(reg, command);
        if (row == nullptr)
        {
            reply["ok"] = false;
            reply["error"] = std::format("unknown command '{}'", command);
            return reply;
        }
        const json params = request.value("params", json::object());
        std::expected<json, std::string> result = row->run(ctx, params);
        if (!result)
        {
            reply["ok"] = false;
            reply["error"] = std::move(result.error());
            return reply;
        }
        reply["ok"] = true;
        reply["result"] = std::move(*result);
        return reply;
    }

    // Accepts pending clients, splits newline-delimited requests, and runs each on
    // the calling thread. Never blocks: recv with MSG_DONTWAIT, replies are single
    // compact json lines, send uses MSG_NOSIGNAL so a vanished client cannot signal.
    void drainControlServer(ControlServer& server, CommandRegistry& reg, EngineContext& ctx)
    {
        for (;;)
        {
            const int clientFd = ::accept4(server.listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (clientFd < 0)
            {
                break;
            }
            server.clients.push_back(ControlClient{ clientFd, {} });
        }

        for (ControlClient& client : server.clients)
        {
            char buffer[4096];
            for (;;)
            {
                const ssize_t received = ::recv(client.fd, buffer, sizeof(buffer), MSG_DONTWAIT);
                if (received > 0)
                {
                    client.inbuf.append(buffer, static_cast<std::size_t>(received));
                    continue;
                }
                if (received == 0)
                {
                    ::close(client.fd);
                    client.fd = -1;
                }
                break;
            }

            std::size_t newline = 0;
            while (client.fd >= 0 && (newline = client.inbuf.find('\n')) != std::string::npos)
            {
                const std::string line = client.inbuf.substr(0, newline);
                client.inbuf.erase(0, newline + 1);

                json request = json::parse(line, nullptr, false);
                json reply;
                if (request.is_discarded())
                {
                    reply = json{ { "ok", false }, { "error", "invalid JSON request" } };
                }
                else
                {
                    reply = dispatch(reg, ctx, request);
                }

                std::string out = reply.dump();
                out.push_back('\n');
                static_cast<void>(::send(client.fd, out.data(), out.size(), MSG_NOSIGNAL));
            }
        }

        std::erase_if(server.clients, [](const ControlClient& client) { return client.fd < 0; });
    }

    /// Owns the command registry + socket. Heap-owned so the client holds only a
    /// pointer (the socket/json members stay out of the client TU).
    struct ControlContext
    {
        CommandRegistry registry;
        ControlServer server;
        bool active = false;
    };

    /// Registers the built-in commands and starts the control socket. A bind
    /// failure is logged and the context returns inactive — the app still runs.
    ControlContext* newControlContext()
    {
        ControlContext* ctx = new ControlContext();
        registerBuiltinCommands(ctx->registry);

        std::expected<ControlServer, std::string> server = startControlServer(controlSocketPath());
        if (server)
        {
            ctx->server = std::move(*server);
            ctx->active = true;
            logInfo(std::format("control socket listening on {}", ctx->server.path));
        }
        else
        {
            logWarn(std::format("control socket disabled: {}", server.error()));
        }
        return ctx;
    }

    void destroyControlContext(ControlContext* ctx)
    {
        if (ctx == nullptr)
        {
            return;
        }
        if (ctx->active)
        {
            stopControlServer(ctx->server);
        }
        delete ctx;
    }

    /// Drains and runs any pending control commands on the calling (main) thread.
    /// Call once per frame.
    void pollControl(ControlContext& ctx, Window& window, Renderer& renderer, EditorContext& editor, AssetServer& assets)
    {
        if (!ctx.active)
        {
            return;
        }
        EngineContext engine{ window, renderer, editor, assets };
        drainControlServer(ctx.server, ctx.registry, engine);
    }
}
