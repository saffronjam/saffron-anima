module;

// Same global-module-fragment shape as the interface unit: Lua headers first
// (no C++ guard), then LuaBridge, classic std includes, no `import std`.
extern "C"
{
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <LuaBridge/LuaBridge.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module Saffron.Script;

import Saffron.Core;
import Saffron.Scene;

namespace se
{
    namespace
    {
        // A read-only JSON -> Lua conversion, total over the component DTO shapes:
        // objects/arrays become tables (arrays 1-based), scalars map 1:1, uuids stay
        // the decimal strings the serde emits, null becomes nil.
        auto jsonToLua(lua_State* L, const nlohmann::json& j) -> luabridge::LuaRef
        {
            if (j.is_object())
            {
                luabridge::LuaRef table = luabridge::newTable(L);
                for (const auto& [key, value] : j.items())
                {
                    table[key] = jsonToLua(L, value);
                }
                return table;
            }
            if (j.is_array())
            {
                luabridge::LuaRef table = luabridge::newTable(L);
                for (std::size_t i = 0; i < j.size(); i += 1)
                {
                    table[i + 1] = jsonToLua(L, j[i]);
                }
                return table;
            }
            if (j.is_string())
            {
                return { L, j.get<std::string>() };
            }
            if (j.is_boolean())
            {
                return { L, j.get<bool>() };
            }
            if (j.is_number_float())
            {
                return { L, j.get<f64>() };
            }
            if (j.is_number_unsigned() && j.get<u64>() > static_cast<u64>(std::numeric_limits<i64>::max()))
            {
                return { L, static_cast<f64>(j.get<u64>()) };
            }
            if (j.is_number_integer() || j.is_number_unsigned())
            {
                return { L, j.get<i64>() };
            }
            return { L };
        }

        // The `self.entity` handle scripts hold: an entt id plus the runtime that
        // resolves it. The scene is reached only through host->currentScene, which
        // is non-null only while a start/tick call is on the stack — so a handle
        // kept past its session degrades to logged no-ops, never a dangling deref.
        struct ScriptEntity
        {
            Entity entity{};
            ScriptHost* host = nullptr;

            auto transformScene(const char* op) const -> Scene*
            {
                if (host == nullptr || host->currentScene == nullptr)
                {
                    logWarn(std::format("script: {} outside a script callback is ignored", op));
                    return nullptr;
                }
                if (!se::valid(*host->currentScene, entity) ||
                    !hasComponent<TransformComponent>(*host->currentScene, entity))
                {
                    logWarn(std::format("script: {} on a missing entity/transform is ignored", op));
                    return nullptr;
                }
                return host->currentScene;
            }

            auto isValid() const -> bool
            {
                return host != nullptr && host->currentScene != nullptr && se::valid(*host->currentScene, entity);
            }

            auto getPosition(lua_State* L) const -> luabridge::LuaRef
            {
                glm::vec3 t{ 0.0f };
                if (Scene* scene = transformScene("get_position"))
                {
                    t = getComponent<TransformComponent>(*scene, entity).translation;
                }
                luabridge::LuaRef p = luabridge::newTable(L);
                p["x"] = t.x;
                p["y"] = t.y;
                p["z"] = t.z;
                return p;
            }

            void setPosition(f32 x, f32 y, f32 z)
            {
                if (Scene* scene = transformScene("set_position"))
                {
                    getComponent<TransformComponent>(*scene, entity).translation = glm::vec3{ x, y, z };
                }
            }

            // Euler XYZ in radians — the local TransformComponent.rotation field.
            void setRotation(f32 rx, f32 ry, f32 rz)
            {
                if (Scene* scene = transformScene("set_rotation"))
                {
                    getComponent<TransformComponent>(*scene, entity).rotation = glm::vec3{ rx, ry, rz };
                }
            }

            void setScale(f32 sx, f32 sy, f32 sz)
            {
                if (Scene* scene = transformScene("set_scale"))
                {
                    getComponent<TransformComponent>(*scene, entity).scale = glm::vec3{ sx, sy, sz };
                }
            }

            auto name() const -> std::string
            {
                if (host == nullptr || host->currentScene == nullptr)
                {
                    return {};
                }
                if (!se::valid(*host->currentScene, entity) ||
                    !hasComponent<NameComponent>(*host->currentScene, entity))
                {
                    return {};
                }
                return getComponent<NameComponent>(*host->currentScene, entity).name;
            }

            // A read-only snapshot of any registered component, via the registry's
            // type-erased serialize — every component is reachable with zero
            // per-type binding code. nil when absent or unknown.
            auto getComponentSnapshot(const char* componentName, lua_State* L) const -> luabridge::LuaRef
            {
                if (host == nullptr || host->currentScene == nullptr || host->currentRegistry == nullptr)
                {
                    logWarn("script: get_component outside a script callback is ignored");
                    return { L };
                }
                Scene& scene = *host->currentScene;
                if (componentName == nullptr || !se::valid(scene, entity))
                {
                    return { L };
                }
                const ComponentTraits* traits = findByName(*host->currentRegistry, componentName);
                if (traits == nullptr || !traits->has(scene, entity))
                {
                    return { L };
                }
                return jsonToLua(L, traits->serialize(scene, entity));
            }
        };

        auto tracebackHandler(lua_State* L) -> int
        {
            const char* message = lua_tostring(L, 1);
            if (message == nullptr)
            {
                message = "unknown script error";
            }
            luaL_traceback(L, L, message, 1);
            return 1;
        }

        auto popError(lua_State* L, int popCount) -> std::string
        {
            std::string error;
            const char* message = lua_tostring(L, -1);
            if (message != nullptr)
            {
                error = message;
            }
            else
            {
                error = "unknown script error";
            }
            lua_pop(L, popCount);
            return error;
        }

        // Calls self:<name>(dt?) for the instance at selfRef. An absent method is
        // a successful no-op (only on_update is required, enforced at class load).
        auto callInstanceMethod(lua_State* L, int selfRef, const char* name, std::optional<f32> dt) -> Result<void>
        {
            lua_pushcfunction(L, tracebackHandler);
            const int msghIndex = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, selfRef);
            lua_getfield(L, -1, name);
            if (!lua_isfunction(L, -1))
            {
                lua_pop(L, 3);
                return {};
            }
            lua_pushvalue(L, -2);
            int nargs = 1;
            if (dt.has_value())
            {
                lua_pushnumber(L, static_cast<lua_Number>(*dt));
                nargs = 2;
            }
            if (lua_pcall(L, nargs, 0, msghIndex) != LUA_OK)
            {
                return Err(popError(L, 3));
            }
            lua_pop(L, 2);
            return {};
        }

        // Loads + runs the script file, which must return a class table carrying
        // on_update. The ref is cached per path for the VM's lifetime.
        auto loadClass(ScriptHost& host, const std::string& path) -> Result<int>
        {
            if (auto it = host.classRefByPath.find(path); it != host.classRefByPath.end())
            {
                return it->second;
            }
            lua_State* L = host.vm.state;
            lua_pushcfunction(L, tracebackHandler);
            const int msghIndex = lua_gettop(L);
            int status = luaL_loadfilex(L, path.c_str(), "t");
            if (status == LUA_OK)
            {
                status = lua_pcall(L, 0, 1, msghIndex);
            }
            if (status != LUA_OK)
            {
                return Err(popError(L, 2));
            }
            if (!lua_istable(L, -1))
            {
                lua_pop(L, 2);
                return Err(std::format("'{}' must return a class table", path));
            }
            lua_getfield(L, -1, "on_update");
            const bool hasUpdate = lua_isfunction(L, -1);
            lua_pop(L, 1);
            if (!hasUpdate)
            {
                lua_pop(L, 2);
                return Err(std::format("'{}' class table has no on_update(self, dt)", path));
            }
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            lua_pop(L, 1);
            host.classRefByPath.emplace(path, ref);
            return ref;
        }

        // Shallow-copies the table at `index` so a table default (vec3) is never
        // shared between instances — mutating self.offset must not bleed across.
        void pushTableCopy(lua_State* L, int index)
        {
            const int source = lua_absindex(L, index);
            lua_createtable(L, static_cast<int>(lua_rawlen(L, source)), 0);
            const int copy = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, source) != 0)
            {
                lua_pushvalue(L, -2);
                lua_pushvalue(L, -2);
                lua_settable(L, copy);
                lua_pop(L, 1);
            }
        }

        // Sets every declared property onto the instance table at selfIndex:
        // the slot's override when present (JSON -> Lua), else the declared
        // default. Unknown override keys are simply never visited — a renamed or
        // removed field's stale override is dropped, never an error.
        void injectFields(lua_State* L, int selfIndex, int classRef, const nlohmann::json& overrides)
        {
            const int self = lua_absindex(L, selfIndex);
            lua_rawgeti(L, LUA_REGISTRYINDEX, classRef);
            lua_getfield(L, -1, "properties");
            if (lua_istable(L, -1))
            {
                const int properties = lua_gettop(L);
                lua_pushnil(L);
                while (lua_next(L, properties) != 0)
                {
                    if (lua_type(L, -2) == LUA_TSTRING)
                    {
                        const char* name = lua_tostring(L, -2);
                        if (overrides.is_object() && overrides.contains(name))
                        {
                            const luabridge::LuaRef value = jsonToLua(L, overrides[name]);
                            value.push();
                        }
                        else if (lua_type(L, -1) == LUA_TTABLE)
                        {
                            pushTableCopy(L, -1);
                        }
                        else
                        {
                            lua_pushvalue(L, -1);
                        }
                        lua_setfield(L, self, name);
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 2);
        }

        // Builds self = setmetatable({ entity = <handle>, <merged fields> },
        // { __index = Class }).
        auto makeInstance(ScriptHost& host, Entity entity, int classRef, const nlohmann::json& overrides) -> Result<int>
        {
            lua_State* L = host.vm.state;
            lua_createtable(L, 0, 1);
            auto pushed = luabridge::push(L, ScriptEntity{ .entity = entity, .host = &host });
            if (!pushed)
            {
                lua_pop(L, 1);
                return Err("failed to push the entity handle");
            }
            lua_setfield(L, -2, "entity");
            injectFields(L, -1, classRef, overrides);
            lua_createtable(L, 0, 1);
            lua_rawgeti(L, LUA_REGISTRYINDEX, classRef);
            lua_setfield(L, -2, "__index");
            lua_setmetatable(L, -2);
            return luaL_ref(L, LUA_REGISTRYINDEX);
        }
    }

    auto startScripts(ScriptHost& host, Scene& scene, const ComponentRegistry& registry, std::string_view srcDir)
        -> Result<void>
    {
        stopScripts(host);
        auto vm = newScriptVm();
        if (!vm)
        {
            return Err(vm.error());
        }
        host.vm = std::move(*vm);
        host.currentRegistry = &registry;
        lua_State* L = host.vm.state;
        luabridge::getGlobalNamespace(L)
            .beginNamespace("se")
            .beginClass<ScriptEntity>("Entity")
            .addFunction("valid", &ScriptEntity::isValid)
            .addFunction("name", &ScriptEntity::name)
            .addFunction("get_component", &ScriptEntity::getComponentSnapshot)
            .addFunction("get_position", &ScriptEntity::getPosition)
            .addFunction("set_position", &ScriptEntity::setPosition)
            .addFunction("set_rotation", &ScriptEntity::setRotation)
            .addFunction("set_scale", &ScriptEntity::setScale)
            .endClass()
            // First match by name (names are not unique — a deliberate MVP choice);
            // an invalid handle when absent, so scripts check :valid().
            .addFunction("get_entity_by_name",
                         [&host](const char* name) -> ScriptEntity
                         {
                             ScriptEntity found{ .entity = Entity{}, .host = &host };
                             if (host.currentScene == nullptr || name == nullptr)
                             {
                                 return found;
                             }
                             forEach<NameComponent>(*host.currentScene,
                                                    [&found, name](Entity entity, NameComponent& nameComponent)
                                                    {
                                                        if (found.entity.handle == entt::null &&
                                                            nameComponent.name == name)
                                                        {
                                                            found.entity = entity;
                                                        }
                                                    });
                             return found;
                         })
            // The scene's first primary CameraComponent entity; moving its transform
            // IS "move camera" (renderCameraView picks it up next frame).
            .addFunction("primary_camera",
                         [&host]() -> ScriptEntity
                         {
                             ScriptEntity found{ .entity = Entity{}, .host = &host };
                             if (host.currentScene == nullptr)
                             {
                                 return found;
                             }
                             forEach<TransformComponent, CameraComponent>(
                                 *host.currentScene,
                                 [&found](Entity entity, TransformComponent&, CameraComponent& camera)
                                 {
                                     if (found.entity.handle == entt::null && camera.primary)
                                     {
                                         found.entity = entity;
                                     }
                                 });
                             return found;
                         })
            .endNamespace();

        host.currentScene = &scene;
        forEach<ScriptComponent>(scene,
                                 [&host, &scene, srcDir](Entity entity, ScriptComponent& component)
                                 {
                                     u64 uuid = 0;
                                     if (hasComponent<IdComponent>(scene, entity))
                                     {
                                         uuid = getComponent<IdComponent>(scene, entity).id.value;
                                     }
                                     for (std::size_t slot = 0; slot < component.scripts.size(); slot += 1)
                                     {
                                         const std::string& rel = component.scripts[slot].scriptPath;
                                         if (rel.empty())
                                         {
                                             continue;
                                         }
                                         const std::string full = (std::filesystem::path(srcDir) / rel).string();
                                         auto classRef = loadClass(host, full);
                                         if (!classRef)
                                         {
                                             logError(std::format("script: skipping '{}': {}", rel, classRef.error()));
                                             continue;
                                         }
                                         auto selfRef =
                                             makeInstance(host, entity, *classRef, component.scripts[slot].overrides);
                                         if (!selfRef)
                                         {
                                             logError(std::format("script: skipping '{}': {}", rel, selfRef.error()));
                                             continue;
                                         }
                                         host.instances.push_back(ScriptInstance{ .entity = entity,
                                                                                  .entityUuid = uuid,
                                                                                  .scriptPath = rel,
                                                                                  .slotIndex = static_cast<i32>(slot),
                                                                                  .selfRef = *selfRef });
                                     }
                                 });
        for (const ScriptInstance& instance : host.instances)
        {
            auto created = callInstanceMethod(L, instance.selfRef, "on_create", std::nullopt);
            if (!created)
            {
                logError(std::format("script: on_create '{}': {}", instance.scriptPath, created.error()));
            }
        }
        host.currentScene = nullptr;
        logInfo(std::format("scripts started: {} instance(s)", host.instances.size()));
        return {};
    }

    auto tickScripts(ScriptHost& host, Scene& scene, f32 dt) -> std::optional<ScriptRunError>
    {
        if (host.vm.state == nullptr || host.instances.empty())
        {
            return std::nullopt;
        }
        host.currentScene = &scene;
        std::optional<ScriptRunError> failure;
        for (const ScriptInstance& instance : host.instances)
        {
            auto ran = callInstanceMethod(host.vm.state, instance.selfRef, "on_update", dt);
            if (!ran)
            {
                failure = ScriptRunError{ .entityUuid = instance.entityUuid,
                                          .script = instance.scriptPath,
                                          .message = std::move(ran.error()) };
                break;
            }
        }
        host.currentScene = nullptr;
        return failure;
    }

    void stopScripts(ScriptHost& host)
    {
        if (host.vm.state != nullptr)
        {
            // The play duplicate may already be discarded; on_destroy runs with no
            // scene bound, so entity access degrades to logged no-ops.
            host.currentScene = nullptr;
            for (const ScriptInstance& instance : host.instances)
            {
                auto destroyed = callInstanceMethod(host.vm.state, instance.selfRef, "on_destroy", std::nullopt);
                if (!destroyed)
                {
                    logWarn(std::format("script: on_destroy '{}': {}", instance.scriptPath, destroyed.error()));
                }
            }
        }
        host.instances.clear();
        host.classRefByPath.clear();
        host.currentRegistry = nullptr;
        host.vm = ScriptVm{};
    }

    auto scriptFieldTypeName(ScriptFieldType type) -> const char*
    {
        switch (type)
        {
        case ScriptFieldType::Bool:
            return "bool";
        case ScriptFieldType::String:
            return "string";
        case ScriptFieldType::Vec3:
            return "vec3";
        case ScriptFieldType::Number:
            break;
        }
        return "number";
    }

    namespace
    {
        // Infers a field from the declared default at the top of the stack:
        // number/bool/string map 1:1, a table of exactly 3 numbers is a vec3
        // (captured as a 3-number JSON array), anything else is not a field.
        auto inferField(lua_State* L, std::string name) -> std::optional<ScriptField>
        {
            const int type = lua_type(L, -1);
            if (type == LUA_TNUMBER)
            {
                return ScriptField{ .name = std::move(name),
                                    .type = ScriptFieldType::Number,
                                    .defaultValue = lua_tonumber(L, -1) };
            }
            if (type == LUA_TBOOLEAN)
            {
                return ScriptField{ .name = std::move(name),
                                    .type = ScriptFieldType::Bool,
                                    .defaultValue = lua_toboolean(L, -1) != 0 };
            }
            if (type == LUA_TSTRING)
            {
                return ScriptField{ .name = std::move(name),
                                    .type = ScriptFieldType::String,
                                    .defaultValue = lua_tostring(L, -1) };
            }
            if (type == LUA_TTABLE && lua_rawlen(L, -1) == 3)
            {
                nlohmann::json vec = nlohmann::json::array();
                for (int i = 1; i <= 3; i += 1)
                {
                    lua_rawgeti(L, -1, i);
                    const bool isNumber = lua_type(L, -1) == LUA_TNUMBER;
                    if (isNumber)
                    {
                        vec.push_back(lua_tonumber(L, -1));
                    }
                    lua_pop(L, 1);
                    if (!isNumber)
                    {
                        return std::nullopt;
                    }
                }
                return ScriptField{ .name = std::move(name), .type = ScriptFieldType::Vec3, .defaultValue = vec };
            }
            return std::nullopt;
        }
    }

    auto readScriptSchema(std::string_view path) -> Result<std::vector<ScriptField>>
    {
        auto vm = newScriptVm();
        if (!vm)
        {
            return Err(vm.error());
        }
        lua_State* L = vm->state;
        lua_pushcfunction(L, tracebackHandler);
        const int msghIndex = lua_gettop(L);
        const std::string file(path);
        int status = luaL_loadfilex(L, file.c_str(), "t");
        if (status == LUA_OK)
        {
            status = lua_pcall(L, 0, 1, msghIndex);
        }
        if (status != LUA_OK)
        {
            return Err(popError(L, 2));
        }
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 2);
            return Err(std::format("'{}' must return a class table", file));
        }
        std::vector<ScriptField> fields;
        lua_getfield(L, -1, "properties");
        if (lua_istable(L, -1))
        {
            const int properties = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, properties) != 0)
            {
                if (lua_type(L, -2) == LUA_TSTRING)
                {
                    auto field = inferField(L, lua_tostring(L, -2));
                    if (field.has_value())
                    {
                        fields.push_back(std::move(*field));
                    }
                    else
                    {
                        logInfo(std::format("script schema '{}': skipping '{}' (uninferable default)", file,
                                            lua_tostring(L, -2)));
                    }
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 3);
        std::ranges::sort(fields, [](const ScriptField& a, const ScriptField& b) { return a.name < b.name; });
        return fields;
    }
}
