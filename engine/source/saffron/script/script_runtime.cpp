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
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
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

        // The inverse of jsonToLua, for set_component: a Lua value at `index` -> JSON. An se.Vec3
        // userdata becomes an { x, y, z } object (the shape vec3FromJson reads, scene.cppm:1121); a
        // string-keyed table -> object; a 1-based sequence -> array; scalars 1:1; everything else -> null.
        auto luaToJson(lua_State* L, int index) -> nlohmann::json
        {
            const int idx = lua_absindex(L, index);
            switch (lua_type(L, idx))
            {
            case LUA_TBOOLEAN:
                return lua_toboolean(L, idx) != 0;
            case LUA_TNUMBER:
                if (lua_isinteger(L, idx) != 0)
                {
                    return static_cast<i64>(lua_tointeger(L, idx));
                }
                return lua_tonumber(L, idx);
            case LUA_TSTRING:
                return std::string{ lua_tostring(L, idx) };
            case LUA_TUSERDATA:
            {
                // An se.Vec3 exposes x/y/z; read them as a { x, y, z } object. Any other userdata
                // (no numeric x/y/z) is dropped to null rather than guessed at.
                nlohmann::json object = nlohmann::json::object();
                for (const char* field : { "x", "y", "z" })
                {
                    lua_getfield(L, idx, field);
                    const bool isNumber = lua_type(L, -1) == LUA_TNUMBER;
                    if (isNumber)
                    {
                        object[field] = lua_tonumber(L, -1);
                    }
                    lua_pop(L, 1);
                    if (!isNumber)
                    {
                        return nullptr;
                    }
                }
                return object;
            }
            case LUA_TTABLE:
            {
                const auto len = static_cast<lua_Integer>(lua_rawlen(L, idx));
                if (len > 0)
                {
                    nlohmann::json array = nlohmann::json::array();
                    for (lua_Integer i = 1; i <= len; i += 1)
                    {
                        lua_rawgeti(L, idx, i);
                        array.push_back(luaToJson(L, -1));
                        lua_pop(L, 1);
                    }
                    return array;
                }
                nlohmann::json object = nlohmann::json::object();
                lua_pushnil(L);
                while (lua_next(L, idx) != 0)
                {
                    if (lua_type(L, -2) == LUA_TSTRING)
                    {
                        object[lua_tostring(L, -2)] = luaToJson(L, -1);
                    }
                    lua_pop(L, 1);
                }
                return object;
            }
            default:
                return nullptr;
            }
        }

        // Cache/asset-backed structural components cooked at play start (the Jolt world, the rig caches,
        // the hierarchy link): a script set/add of one mid-play would desync the live state, so the write
        // bindings refuse them (the registry's deserialize auto-adds, which is correct only for scene load).
        // Keyed on the registered NAME — verify against scene_edit_components.cpp registerBuiltinComponents.
        constexpr std::array kStructuralComponents = { "Relationship", "SkinnedMesh", "Bone",      "FootIk",
                                                       "BonePhysics",  "Collider",    "Rigidbody", "KinematicBones" };

        auto isStructuralComponent(std::string_view name) -> bool
        {
            return std::ranges::find(kStructuralComponents, name) != kStructuralComponents.end();
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
                    !se::hasComponent<TransformComponent>(*host->currentScene, entity))
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

            // Transforms cross the boundary as se.Vec3 (a glm::vec3 userdata); rotation is euler radians.
            auto getPosition() const -> glm::vec3
            {
                Scene* scene = transformScene("get_position");
                return scene != nullptr ? getComponent<TransformComponent>(*scene, entity).translation
                                        : glm::vec3{ 0.0f };
            }

            auto getRotation() const -> glm::vec3
            {
                Scene* scene = transformScene("get_rotation");
                return scene != nullptr ? getComponent<TransformComponent>(*scene, entity).rotation : glm::vec3{ 0.0f };
            }

            auto getScale() const -> glm::vec3
            {
                Scene* scene = transformScene("get_scale");
                return scene != nullptr ? getComponent<TransformComponent>(*scene, entity).scale : glm::vec3{ 1.0f };
            }

            // World space (composed through the hierarchy); rotation decomposed to euler radians so it
            // round-trips through set_rotation.
            auto getWorldPosition() const -> glm::vec3
            {
                Scene* scene = transformScene("get_world_position");
                return scene != nullptr ? worldTranslation(*scene, entity) : glm::vec3{ 0.0f };
            }

            auto getWorldRotation() const -> glm::vec3
            {
                Scene* scene = transformScene("get_world_rotation");
                return scene != nullptr ? quatToEulerZYX(worldRotation(*scene, entity)) : glm::vec3{ 0.0f };
            }

            void setPosition(const glm::vec3& v)
            {
                if (Scene* scene = transformScene("set_position"))
                {
                    getComponent<TransformComponent>(*scene, entity).translation = v;
                }
            }

            void setRotation(const glm::vec3& v)
            {
                if (Scene* scene = transformScene("set_rotation"))
                {
                    getComponent<TransformComponent>(*scene, entity).rotation = v;
                }
            }

            void setScale(const glm::vec3& v)
            {
                if (Scene* scene = transformScene("set_scale"))
                {
                    getComponent<TransformComponent>(*scene, entity).scale = v;
                }
            }

            auto name() const -> std::string
            {
                if (host == nullptr || host->currentScene == nullptr)
                {
                    return {};
                }
                if (!se::valid(*host->currentScene, entity) ||
                    !se::hasComponent<NameComponent>(*host->currentScene, entity))
                {
                    return {};
                }
                return getComponent<NameComponent>(*host->currentScene, entity).name;
            }

            auto uuid() const -> std::string
            {
                if (host == nullptr || host->currentScene == nullptr || !se::valid(*host->currentScene, entity) ||
                    !se::hasComponent<IdComponent>(*host->currentScene, entity))
                {
                    return "0";
                }
                return std::to_string(getComponent<IdComponent>(*host->currentScene, entity).id.value);
            }

            // The scene + registry, valid only inside a callback on a live entity; else a logged nullptr.
            // The shared guard behind every component read/write.
            auto registryScene(const char* op) const -> Scene*
            {
                if (host == nullptr || host->currentScene == nullptr || host->currentRegistry == nullptr)
                {
                    logWarn(std::format("script: {} outside a script callback is ignored", op));
                    return nullptr;
                }
                if (!se::valid(*host->currentScene, entity))
                {
                    logWarn(std::format("script: {} on a dead entity is ignored", op));
                    return nullptr;
                }
                return host->currentScene;
            }

            // A read-only snapshot of any registered component, via the registry's type-erased serialize —
            // every component reachable with zero per-type code. nil when absent or unknown.
            auto getComponentSnapshot(const char* componentName, lua_State* L) const -> luabridge::LuaRef
            {
                Scene* scene = registryScene("get_component");
                if (scene == nullptr || componentName == nullptr)
                {
                    return { L };
                }
                const ComponentTraits* traits = findByName(*host->currentRegistry, componentName);
                if (traits == nullptr || !traits->has(*scene, entity))
                {
                    return { L };
                }
                return jsonToLua(L, traits->serialize(*scene, entity));
            }

            // The write mirror of get_component: a Lua table (or se.Vec3) -> the registry's deserialize.
            // Refuses cache-backed structural components (the gate); an unknown name or a deserialize
            // failure is a logged false. deserialize merges onto the live component (partial patches work).
            auto setComponent(const char* componentName, luabridge::LuaRef value) const -> bool
            {
                Scene* scene = registryScene("set_component");
                if (scene == nullptr || componentName == nullptr)
                {
                    return false;
                }
                if (isStructuralComponent(componentName))
                {
                    logWarn(std::format("script: set_component('{}') refused (structural component)", componentName));
                    return false;
                }
                const ComponentTraits* traits = findByName(*host->currentRegistry, componentName);
                if (traits == nullptr)
                {
                    logWarn(std::format("script: set_component('{}') — unknown component", componentName));
                    return false;
                }
                lua_State* L = value.state();
                value.push();
                const nlohmann::json json = luaToJson(L, -1);
                lua_pop(L, 1);
                auto deserialized = traits->deserialize(*scene, entity, json);
                if (!deserialized)
                {
                    logWarn(std::format("script: set_component('{}'): {}", componentName, deserialized.error()));
                    return false;
                }
                return true;
            }

            auto addComponent(const char* componentName) const -> bool
            {
                Scene* scene = registryScene("add_component");
                if (scene == nullptr || componentName == nullptr)
                {
                    return false;
                }
                if (isStructuralComponent(componentName))
                {
                    logWarn(std::format("script: add_component('{}') refused (structural component)", componentName));
                    return false;
                }
                const ComponentTraits* traits = findByName(*host->currentRegistry, componentName);
                if (traits == nullptr || traits->has(*scene, entity))
                {
                    return false;
                }
                traits->addDefault(*scene, entity);
                return true;
            }

            auto removeComponent(const char* componentName) const -> bool
            {
                Scene* scene = registryScene("remove_component");
                if (scene == nullptr || componentName == nullptr)
                {
                    return false;
                }
                const ComponentTraits* traits = findByName(*host->currentRegistry, componentName);
                if (traits == nullptr || !traits->removable || !traits->has(*scene, entity))
                {
                    return false;
                }
                traits->remove(*scene, entity);
                return true;
            }

            auto hasComponent(const char* componentName) const -> bool
            {
                Scene* scene = registryScene("has_component");
                if (scene == nullptr || componentName == nullptr)
                {
                    return false;
                }
                const ComponentTraits* traits = findByName(*host->currentRegistry, componentName);
                return traits != nullptr && traits->has(*scene, entity);
            }

            // Queue this entity (and its subtree) for destruction after the instance loop, so the handle
            // stays valid for the rest of the current handler. :valid() flips false once the flush runs.
            void destroy() const
            {
                if (host == nullptr || host->currentScene == nullptr || !se::valid(*host->currentScene, entity) ||
                    !se::hasComponent<IdComponent>(*host->currentScene, entity))
                {
                    logWarn("script: destroy outside a callback / on a dead entity is ignored");
                    return;
                }
                host->pendingDestroy.push_back(getComponent<IdComponent>(*host->currentScene, entity).id.value);
                host->hierarchyDirty = true;
            }

            // The only reparent path: runs setParent (which guards self/cycle/dangling and relinks). Safe
            // mid-tick (setParent touches components + entt views, not the instance vector being iterated).
            auto setParent(const ScriptEntity& other) const -> bool
            {
                if (host == nullptr || host->currentScene == nullptr || !se::valid(*host->currentScene, entity))
                {
                    logWarn("script: set_parent outside a callback / on a dead entity is ignored");
                    return false;
                }
                auto reparented = se::setParent(*host->currentScene, entity, other.entity);
                if (!reparented)
                {
                    logWarn(std::format("script: set_parent: {}", reparented.error()));
                    return false;
                }
                return true;
            }

            // The parent handle, or an invalid handle at the root (check :valid()).
            auto parent() const -> ScriptEntity
            {
                ScriptEntity none{ .entity = Entity{}, .host = host };
                if (host == nullptr || host->currentScene == nullptr || !se::valid(*host->currentScene, entity) ||
                    !se::hasComponent<RelationshipComponent>(*host->currentScene, entity))
                {
                    return none;
                }
                const Uuid parentId = getComponent<RelationshipComponent>(*host->currentScene, entity).parent;
                if (parentId.value == 0)
                {
                    return none;
                }
                return ScriptEntity{ .entity = findEntityByUuid(*host->currentScene, parentId.value), .host = host };
            }

            auto children() const -> luabridge::LuaRef
            {
                luabridge::LuaRef array = luabridge::newTable(host != nullptr ? host->vm.state : nullptr);
                if (host == nullptr || host->currentScene == nullptr || !se::valid(*host->currentScene, entity) ||
                    !se::hasComponent<RelationshipComponent>(*host->currentScene, entity))
                {
                    return array;
                }
                int i = 1;
                for (const entt::entity child :
                     getComponent<RelationshipComponent>(*host->currentScene, entity).children)
                {
                    array[i] = ScriptEntity{ .entity = Entity{ child }, .host = host };
                    i += 1;
                }
                return array;
            }

            // Queue a message to this entity's scripts: self:<handler>(sender, payload) is invoked after the
            // instance loop. `payload` may be nil. The sender is the instance whose handler is running.
            void send(const char* handler, luabridge::LuaRef payload) const
            {
                if (host == nullptr || host->currentScene == nullptr || handler == nullptr ||
                    !se::valid(*host->currentScene, entity) ||
                    !se::hasComponent<IdComponent>(*host->currentScene, entity))
                {
                    logWarn("script: send outside a callback / on a dead entity is ignored");
                    return;
                }
                int payloadRef = LUA_NOREF;
                if (!payload.isNil())
                {
                    payload.push();
                    payloadRef = luaL_ref(payload.state(), LUA_REGISTRYINDEX);
                }
                host->messages.push_back(
                    ScriptMessage{ .target = getComponent<IdComponent>(*host->currentScene, entity).id.value,
                                   .sender = host->currentSenderUuid,
                                   .handler = handler,
                                   .payloadRef = payloadRef });
            }

            // The entity's uuid as a u64, or 0 (outside a callback, dead, or no IdComponent). The key
            // every physics bridge passes — the live world maps it back to the Jolt body.
            auto idValue() const -> u64
            {
                if (host == nullptr || host->currentScene == nullptr || !se::valid(*host->currentScene, entity) ||
                    !se::hasComponent<IdComponent>(*host->currentScene, entity))
                {
                    return 0;
                }
                return getComponent<IdComponent>(*host->currentScene, entity).id.value;
            }

            // Drive a CharacterController capsule: desired horizontal velocity (y ignored) consumed by the
            // next physics step; jump applies the fixed vertical impulse. A pure Scene write — the controller
            // component lives in Saffron.Scene, so no physics bridge is needed.
            void moveCharacter(const glm::vec3& velocity, bool jump) const
            {
                if (host == nullptr || host->currentScene == nullptr || !se::valid(*host->currentScene, entity) ||
                    !se::hasComponent<CharacterControllerComponent>(*host->currentScene, entity))
                {
                    logWarn("script: move_character on an entity without a CharacterController is ignored");
                    return;
                }
                auto& controller = getComponent<CharacterControllerComponent>(*host->currentScene, entity);
                controller.desiredVelocity = glm::vec3(velocity.x, 0.0f, velocity.z);
                if (jump)
                {
                    controller.verticalVelocity = 5.0f;
                }
            }

            // Push a Dynamic rigidbody (between steps): impulse / continuous force / absolute velocity, and
            // read its velocity back. Bridges over Saffron.Physics; a non-Dynamic body is a no-op there.
            void applyImpulse(const glm::vec3& impulse) const
            {
                if (host != nullptr && host->applyImpulse)
                {
                    host->applyImpulse(idValue(), impulse);
                }
            }
            void addForce(const glm::vec3& force) const
            {
                if (host != nullptr && host->addForce)
                {
                    host->addForce(idValue(), force);
                }
            }
            void setVelocity(const glm::vec3& velocity) const
            {
                if (host != nullptr && host->setVelocity)
                {
                    host->setVelocity(idValue(), velocity);
                }
            }
            auto getVelocity() const -> glm::vec3
            {
                return host != nullptr && host->getVelocity ? host->getVelocity(idValue()) : glm::vec3{ 0.0f };
            }

            // Ragdoll control (bridges over Saffron.Physics): go limp / restore, blend physics vs animation,
            // and read the live state. The rig uuid is this entity's id.
            auto enableRagdoll() const -> bool
            {
                return host != nullptr && host->setRagdollEnabled && host->setRagdollEnabled(idValue(), true);
            }
            void disableRagdoll() const
            {
                if (host != nullptr && host->setRagdollEnabled)
                {
                    host->setRagdollEnabled(idValue(), false);
                }
            }
            void setRagdollBlend(bool active, f32 weight) const
            {
                if (host != nullptr && host->setRagdollBlend)
                {
                    host->setRagdollBlend(idValue(), active, weight);
                }
            }
            auto ragdollState(lua_State* L) const -> luabridge::LuaRef
            {
                luabridge::LuaRef table = luabridge::newTable(L);
                const ScriptRagdollState state =
                    host != nullptr && host->ragdollState ? host->ragdollState(idValue()) : ScriptRagdollState{};
                table["present"] = state.present;
                table["active"] = state.active;
                table["bodyWeight"] = state.bodyWeight;
                table["bones"] = state.bones;
                return table;
            }
        };

        // Apply queued structural ops after an instance loop, then relink once if anything changed —
        // never mid-loop (the instance vector is iterated by reference). createEntity/setParent already
        // ran inline (safe); only destroy is queued (so a self-destroy stays valid for its handler).
        void flushStructuralOps(ScriptHost& host, Scene& scene)
        {
            for (const u64 uuid : host.pendingDestroy)
            {
                const Entity entity = findEntityByUuid(scene, uuid);
                if (se::valid(scene, entity))
                {
                    destroyEntity(scene, entity);
                }
            }
            host.pendingDestroy.clear();
            if (host.hierarchyDirty)
            {
                relinkHierarchy(scene);
                host.hierarchyDirty = false;
            }
        }

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

        auto normalizeInputKey(std::string key) -> std::string
        {
            std::ranges::transform(key, key.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return key;
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

        // Calls self:<name>(other, [point, normal]) for the instance at selfRef. An absent method is
        // a successful no-op (contact handlers are all optional). Mirrors callInstanceMethod's stack.
        auto callContactHandler(ScriptHost& host, int selfRef, const char* name, Entity other, bool withManifold,
                                f32 px, f32 py, f32 pz, f32 nx, f32 ny, f32 nz) -> Result<void>
        {
            lua_State* L = host.vm.state;
            lua_pushcfunction(L, tracebackHandler);
            const int msghIndex = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, selfRef);
            lua_getfield(L, -1, name);
            if (!lua_isfunction(L, -1))
            {
                lua_pop(L, 3);  // nil handler, self, msgh
                return {};
            }
            lua_pushvalue(L, -2);                                                              // self (arg 1)
            auto pushed = luabridge::push(L, ScriptEntity{ .entity = other, .host = &host });  // other (arg 2)
            static_cast<void>(pushed);
            int nargs = 2;
            if (withManifold)
            {
                static_cast<void>(luabridge::push(L, glm::vec3{ px, py, pz }));  // point (arg 3)
                static_cast<void>(luabridge::push(L, glm::vec3{ nx, ny, nz }));  // normal (arg 4)
                nargs = 4;
            }
            if (lua_pcall(L, nargs, 0, msghIndex) != LUA_OK)
            {
                return Err(popError(L, 3));  // error, self, msgh
            }
            lua_pop(L, 2);  // self, msgh
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
        // True when the value at `index` is an se.Vec3 userdata (has numeric x/y/z).
        auto isVec3Userdata(lua_State* L, int index) -> bool
        {
            if (lua_type(L, index) != LUA_TUSERDATA)
            {
                return false;
            }
            const int idx = lua_absindex(L, index);
            bool ok = true;
            for (const char* field : { "x", "y", "z" })
            {
                lua_getfield(L, idx, field);
                ok = ok && lua_type(L, -1) == LUA_TNUMBER;
                lua_pop(L, 1);
            }
            return ok;
        }

        // Read a glm::vec3 off a value with numeric x/y/z (an se.Vec3 userdata) at `index`.
        auto readVec3Userdata(lua_State* L, int index) -> glm::vec3
        {
            const int idx = lua_absindex(L, index);
            glm::vec3 v{ 0.0f };
            lua_getfield(L, idx, "x");
            v.x = static_cast<f32>(lua_tonumber(L, -1));
            lua_getfield(L, idx, "y");
            v.y = static_cast<f32>(lua_tonumber(L, -1));
            lua_getfield(L, idx, "z");
            v.z = static_cast<f32>(lua_tonumber(L, -1));
            lua_pop(L, 3);
            return v;
        }

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
                        // A vec3 field (an se.Vec3 default) injects a fresh per-instance Vec3 — from the
                        // override's 3-number array when present, else a value-copy of the default (so a
                        // mutated self.offset never aliases across instances). Tables copy; scalars push.
                        const bool vec3Field = isVec3Userdata(L, -1);
                        if (overrides.is_object() && overrides.contains(name))
                        {
                            const nlohmann::json& override = overrides[name];
                            if (vec3Field && override.is_array() && override.size() == 3)
                            {
                                static_cast<void>(
                                    luabridge::push(L, glm::vec3{ override[0].get<f32>(), override[1].get<f32>(),
                                                                  override[2].get<f32>() }));
                            }
                            else
                            {
                                const luabridge::LuaRef value = jsonToLua(L, override);
                                value.push();
                            }
                        }
                        else if (vec3Field)
                        {
                            static_cast<void>(luabridge::push(L, readVec3Userdata(L, -1)));
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

        // The Roblox-task-style coroutine scheduler, pure Lua over the enabled coroutine lib. se.wait
        // yields the running coroutine (a no-op outside one, never a tick error); the host resumes ready
        // ones each tick via _se_advance(dt), timed off accumulated dt (deterministic, never os.clock).
        // `se` is a LuaBridge namespace table with a read-only __newindex, so the scheduler functions are
        // installed with rawset (a plain `se.wait = …` would error). _se_advance is an ordinary global.
        constexpr std::string_view SchedulerPrelude = R"(
local _tasks, _accum = {}, 0
rawset(se, "spawn_task", function(fn, ...)
  local co = coroutine.create(fn)
  local ok, waitFor = coroutine.resume(co, ...)
  if not ok then se.log("se: task error: " .. tostring(waitFor))
  elseif coroutine.status(co) ~= "dead" then
    _tasks[#_tasks + 1] = { co = co, wake = _accum + (type(waitFor) == "number" and waitFor or 0) }
  end
  return co
end)
rawset(se, "wait", function(seconds)
  local _, ismain = coroutine.running()
  if ismain then se.log("se.wait called outside a coroutine is ignored") return end
  return coroutine.yield(seconds or 0)
end)
rawset(se, "delay", function(seconds, fn)
  return se.spawn_task(function() se.wait(seconds) fn() end)
end)
function _se_advance(dt)
  _accum = _accum + dt
  local ready, keep = {}, {}
  for _, t in ipairs(_tasks) do
    if t.wake <= _accum then ready[#ready + 1] = t else keep[#keep + 1] = t end
  end
  _tasks = keep
  for _, t in ipairs(ready) do
    local ok, waitFor = coroutine.resume(t.co)
    if not ok then se.log("se: coroutine error: " .. tostring(waitFor))
    elseif coroutine.status(t.co) ~= "dead" then
      _tasks[#_tasks + 1] = { co = t.co, wake = _accum + (type(waitFor) == "number" and waitFor or 0) }
    end
  end
end
)";

        // Resume ready coroutines (the scheduler) by dt, inside the tick window. Contained under the
        // traceback handler; a faulting coroutine logs, never crashes the VM (mirrors callInstanceMethod).
        void advanceScheduler(ScriptHost& host, f32 dt)
        {
            lua_State* L = host.vm.state;
            lua_pushcfunction(L, tracebackHandler);
            const int msghIndex = lua_gettop(L);
            lua_getglobal(L, "_se_advance");
            if (lua_isfunction(L, -1) == 0)
            {
                lua_pop(L, 2);  // non-function, msgh
                return;
            }
            lua_pushnumber(L, static_cast<lua_Number>(dt));
            if (lua_pcall(L, 1, 0, msghIndex) != LUA_OK)
            {
                logWarn(std::format("se: scheduler: {}", popError(L, 1)));  // pops the error
            }
            lua_pop(L, 1);  // msgh
        }

        // Invoke self:<name>(sender, payload) for one instance, contained (a fault logs, never aborts the
        // rest). Mirrors callContactHandler's stack discipline.
        void callMessageHandler(ScriptHost& host, int selfRef, const std::string& name, Entity sender, int payloadRef)
        {
            lua_State* L = host.vm.state;
            lua_pushcfunction(L, tracebackHandler);
            const int msghIndex = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, selfRef);
            lua_getfield(L, -1, name.c_str());
            if (lua_isfunction(L, -1) == 0)
            {
                lua_pop(L, 3);  // nil handler, self, msgh
                return;
            }
            lua_pushvalue(L, -2);                                                                    // self (arg 1)
            static_cast<void>(luabridge::push(L, ScriptEntity{ .entity = sender, .host = &host }));  // sender (arg 2)
            if (payloadRef != LUA_NOREF)
            {
                lua_rawgeti(L, LUA_REGISTRYINDEX, payloadRef);  // payload (arg 3)
            }
            else
            {
                lua_pushnil(L);
            }
            if (lua_pcall(L, 3, 0, msghIndex) != LUA_OK)
            {
                logWarn(std::format("se: message '{}': {}", name, popError(L, 3)));  // error, self, msgh
                return;
            }
            lua_pop(L, 2);  // self, msgh
        }

        // Drain queued messages after the instance loop (entity:send / se.broadcast), dispatching to each
        // matching instance, then release each payload ref. Never runs mid-loop (the instance vector is
        // iterated by reference).
        void dispatchMessages(ScriptHost& host, Scene& scene)
        {
            if (host.messages.empty())
            {
                return;
            }
            const std::vector<ScriptMessage> pending = std::move(host.messages);
            host.messages.clear();
            for (const ScriptMessage& message : pending)
            {
                const Entity sender = message.sender != 0 ? findEntityByUuid(scene, message.sender) : Entity{};
                for (const ScriptInstance& instance : host.instances)
                {
                    if (message.target != 0 && instance.entityUuid != message.target)
                    {
                        continue;
                    }
                    callMessageHandler(host, instance.selfRef, message.handler, sender, message.payloadRef);
                }
                if (message.payloadRef != LUA_NOREF)
                {
                    luaL_unref(host.vm.state, LUA_REGISTRYINDEX, message.payloadRef);
                }
            }
        }
    }

    // The pure value types + math helpers, bound into BOTH the runtime VM (startScripts) and the
    // throwaway schema VM (newScriptVm) — a `properties` default of se.vec3(0,1,0) must resolve at edit
    // time too. No host closure, so it binds cleanly in either VM.
    void registerScriptValueTypes(lua_State* L)
    {
        luabridge::getGlobalNamespace(L)
            .beginNamespace("se")
            // se.Vec3 is a glm::vec3-backed value userdata. addPropertyReadWrite (not the read-only
            // single-arg addProperty) so `v.x = 5` writes through; the __mul overload set registers both
            // operand orders (Lua dispatches scalar*vec on the right operand). &glm::vec3::x relies on GLM's
            // named-member layout (no GLM_FORCE_SWIZZLE in cmake/Dependencies.cmake).
            .beginClass<glm::vec3>("Vec3")
            .addPropertyReadWrite("x", &glm::vec3::x)
            .addPropertyReadWrite("y", &glm::vec3::y)
            .addPropertyReadWrite("z", &glm::vec3::z)
            .addStaticFunction(
                "new", +[](f32 x, f32 y, f32 z) { return glm::vec3{ x, y, z }; })
            .addFunction(
                "__add", +[](const glm::vec3& a, const glm::vec3& b) { return a + b; })
            .addFunction(
                "__sub", +[](const glm::vec3& a, const glm::vec3& b) { return a - b; })
            // A raw cfunction so __mul handles BOTH operand orders: Lua calls __mul(a, b) in source order,
            // and for `scalar * vec` the left operand is a number. A class member-function form rejects a
            // non-class first arg, so dispatch on which operand is the number.
            .addFunction(
                "__mul",
                +[](lua_State* L) -> int
                {
                    const bool scalarFirst = lua_isnumber(L, 1) != 0 && lua_type(L, 2) == LUA_TUSERDATA;
                    const glm::vec3 v = readVec3Userdata(L, scalarFirst ? 2 : 1);
                    const auto s = static_cast<f32>(lua_tonumber(L, scalarFirst ? 1 : 2));
                    static_cast<void>(luabridge::push(L, v * s));
                    return 1;
                })
            .addFunction(
                "__unm", +[](const glm::vec3& a) { return -a; })
            .addFunction(
                "__eq", +[](const glm::vec3& a, const glm::vec3& b) { return a == b; })
            .addFunction(
                "__tostring", +[](const glm::vec3& a) { return std::format("Vec3({}, {}, {})", a.x, a.y, a.z); })
            .addFunction(
                "length", +[](const glm::vec3& a) { return glm::length(a); })
            .addFunction(
                "normalized", +[](const glm::vec3& a) { return glm::normalize(a); })
            .addFunction(
                "dot", +[](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); })
            .addFunction(
                "cross", +[](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); })
            .addFunction(
                "lerp", +[](const glm::vec3& a, const glm::vec3& b, f32 t) { return glm::mix(a, b, t); })
            .endClass()
            .addFunction(
                "vec3", +[](f32 x, f32 y, f32 z) { return glm::vec3{ x, y, z }; })
            .addFunction(
                "lerp", +[](const glm::vec3& a, const glm::vec3& b, f32 t) { return glm::mix(a, b, t); })
            // A look rotation (euler radians, so it feeds set_rotation): face `target` from `eye`, `up` the
            // reference. Degenerate (eye == target) returns zero.
            .addFunction(
                "look_at",
                +[](const glm::vec3& eye, const glm::vec3& target, const glm::vec3& up) -> glm::vec3
                {
                    const glm::vec3 dir = target - eye;
                    if (glm::length(dir) < 1e-6f)
                    {
                        return glm::vec3{ 0.0f };
                    }
                    return quatToEulerZYX(glm::quatLookAt(glm::normalize(dir), up));
                })
            .endNamespace();
    }

    auto startScripts(ScriptHost& host, Scene& scene, const ComponentRegistry& registry, std::string_view srcDir,
                      const ScriptInputState& input) -> Result<void>
    {
        stopScripts(host);
        auto vm = newScriptVm();
        if (!vm)
        {
            return Err(vm.error());
        }
        host.vm = std::move(*vm);
        host.currentRegistry = &registry;
        host.input = &input;
        lua_State* L = host.vm.state;
        registerScriptValueTypes(L);
        luabridge::getGlobalNamespace(L)
            .beginNamespace("se")
            .beginClass<ScriptEntity>("Entity")
            .addFunction("valid", &ScriptEntity::isValid)
            .addFunction("name", &ScriptEntity::name)
            .addFunction("uuid", &ScriptEntity::uuid)
            .addFunction("get_component", &ScriptEntity::getComponentSnapshot)
            .addFunction("set_component", &ScriptEntity::setComponent)
            .addFunction("add_component", &ScriptEntity::addComponent)
            .addFunction("remove_component", &ScriptEntity::removeComponent)
            .addFunction("has_component", &ScriptEntity::hasComponent)
            .addFunction("get_position", &ScriptEntity::getPosition)
            .addFunction("get_rotation", &ScriptEntity::getRotation)
            .addFunction("get_scale", &ScriptEntity::getScale)
            .addFunction("get_world_position", &ScriptEntity::getWorldPosition)
            .addFunction("get_world_rotation", &ScriptEntity::getWorldRotation)
            .addFunction("set_position", &ScriptEntity::setPosition)
            .addFunction("set_rotation", &ScriptEntity::setRotation)
            .addFunction("set_scale", &ScriptEntity::setScale)
            .addFunction("destroy", &ScriptEntity::destroy)
            .addFunction("set_parent", &ScriptEntity::setParent)
            .addFunction("parent", &ScriptEntity::parent)
            .addFunction("children", &ScriptEntity::children)
            .addFunction("send", &ScriptEntity::send)
            .addFunction("move_character", &ScriptEntity::moveCharacter)
            .addFunction("apply_impulse", &ScriptEntity::applyImpulse)
            .addFunction("add_force", &ScriptEntity::addForce)
            .addFunction("set_velocity", &ScriptEntity::setVelocity)
            .addFunction("get_velocity", &ScriptEntity::getVelocity)
            .addFunction("enable_ragdoll", &ScriptEntity::enableRagdoll)
            .addFunction("disable_ragdoll", &ScriptEntity::disableRagdoll)
            .addFunction("set_ragdoll_blend", &ScriptEntity::setRagdollBlend)
            .addFunction("ragdoll_state", &ScriptEntity::ragdollState)
            .endClass()
            // Held state: true every tick the key is down (the UE "IsKeyDown" sense).
            .addFunction("is_key_down",
                         [&host](const char* key) -> bool
                         {
                             return host.input != nullptr && key != nullptr &&
                                    host.input->held.contains(normalizeInputKey(key));
                         })
            // Press edge (derived per tick): true the one tick the key goes down, then false until released.
            .addFunction("is_key_pressed",
                         [&host](const char* key) -> bool
                         {
                             return host.input != nullptr && key != nullptr &&
                                    host.input->pressed.contains(normalizeInputKey(key));
                         })
            // Release edge: true the one tick the key goes up.
            .addFunction("is_key_up",
                         [&host](const char* key) -> bool
                         {
                             return host.input != nullptr && key != nullptr &&
                                    host.input->released.contains(normalizeInputKey(key));
                         })
            .addFunction("mouse_position",
                         [&host]() -> glm::vec3
                         {
                             return host.input != nullptr ? glm::vec3{ host.input->mouseX, host.input->mouseY, 0.0f }
                                                          : glm::vec3{ 0.0f };
                         })
            .addFunction("mouse_delta",
                         [&host]() -> glm::vec3
                         {
                             return host.input != nullptr ? glm::vec3{ host.input->mouseDX, host.input->mouseDY, 0.0f }
                                                          : glm::vec3{ 0.0f };
                         })
            // Mouse buttons mirror the key trio: down = held, pressed/up = the one-tick edges.
            .addFunction("is_mouse_down",
                         [&host](const char* button) -> bool
                         {
                             return host.input != nullptr && button != nullptr &&
                                    host.input->mouseButtons.contains(normalizeInputKey(button));
                         })
            .addFunction("is_mouse_pressed",
                         [&host](const char* button) -> bool
                         {
                             return host.input != nullptr && button != nullptr &&
                                    host.input->mousePressed.contains(normalizeInputKey(button));
                         })
            .addFunction("is_mouse_up",
                         [&host](const char* button) -> bool
                         {
                             return host.input != nullptr && button != nullptr &&
                                    host.input->mouseReleased.contains(normalizeInputKey(button));
                         })
            .addFunction("mouse_scroll", [&host]() -> f32 { return host.input != nullptr ? host.input->scroll : 0.0f; })
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
            // Mint a new entity (Name + Transform + Relationship, a root) in the play duplicate. It is
            // discarded on stop like everything else; a ScriptComponent added to it runs only next play.
            .addFunction("spawn",
                         [&host](const char* name) -> ScriptEntity
                         {
                             if (host.currentScene == nullptr || name == nullptr)
                             {
                                 return ScriptEntity{ .entity = Entity{}, .host = &host };
                             }
                             return ScriptEntity{ .entity = createEntity(*host.currentScene, name), .host = &host };
                         })
            // Every entity matching `name` (the multi-match get_entity_by_name cannot give).
            .addFunction("find_all_by_name",
                         [&host](const char* name) -> luabridge::LuaRef
                         {
                             luabridge::LuaRef array = luabridge::newTable(host.vm.state);
                             if (host.currentScene == nullptr || name == nullptr)
                             {
                                 return array;
                             }
                             int i = 1;
                             forEach<NameComponent>(*host.currentScene,
                                                    [&](Entity entity, NameComponent& nameComponent)
                                                    {
                                                        if (nameComponent.name == name)
                                                        {
                                                            array[i] = ScriptEntity{ .entity = entity, .host = &host };
                                                            i += 1;
                                                        }
                                                    });
                             return array;
                         })
            // Resolve a uuid (decimal string, matching :uuid()) to its entity, or an invalid handle.
            .addFunction("find_by_uuid",
                         [&host](const char* uuid) -> ScriptEntity
                         {
                             ScriptEntity none{ .entity = Entity{}, .host = &host };
                             if (host.currentScene == nullptr || uuid == nullptr)
                             {
                                 return none;
                             }
                             u64 id = 0;
                             const std::string_view text{ uuid };
                             std::from_chars(text.data(), text.data() + text.size(), id);
                             if (id == 0)
                             {
                                 return none;
                             }
                             return ScriptEntity{ .entity = findEntityByUuid(*host.currentScene, id), .host = &host };
                         })
            // Queue a message to every script instance: handler(self, sender, payload) after the loop.
            .addFunction(
                "broadcast",
                [&host](const char* handler, luabridge::LuaRef payload)
                {
                    if (handler == nullptr)
                    {
                        return;
                    }
                    int payloadRef = LUA_NOREF;
                    if (!payload.isNil())
                    {
                        payload.push();
                        payloadRef = luaL_ref(payload.state(), LUA_REGISTRYINDEX);
                    }
                    host.messages.push_back(ScriptMessage{
                        .target = 0, .sender = host.currentSenderUuid, .handler = handler, .payloadRef = payloadRef });
                })
            // Override newScriptVm's plain log on the play VM: also route the line to the Host's logSink
            // (the editor's script-log ring), tagged with the entity whose handler is running.
            .addFunction("log",
                         [&host](const char* message)
                         {
                             if (message == nullptr)
                             {
                                 return;
                             }
                             logInfo(message);
                             if (host.logSink)
                             {
                                 host.logSink(host.currentSenderUuid, message);
                             }
                         })
            // Cast a ray against the live physics world: se.raycast(ox,oy,oz, dx,dy,dz, maxDist)
            // returns { hit, distance, point=se.Vec3, normal=se.Vec3, entity=<se.Entity or nil> }.
            .addFunction(
                "raycast",
                [&host](float ox, float oy, float oz, float dx, float dy, float dz, float maxDist) -> luabridge::LuaRef
                {
                    luabridge::LuaRef result = luabridge::newTable(host.vm.state);
                    if (!host.raycast)
                    {
                        result["hit"] = false;
                        return result;
                    }
                    const ScriptRayHit hit = host.raycast(ox, oy, oz, dx, dy, dz, maxDist);
                    result["hit"] = hit.hit;
                    result["distance"] = hit.distance;
                    result["point"] = glm::vec3{ hit.px, hit.py, hit.pz };
                    result["normal"] = glm::vec3{ hit.nx, hit.ny, hit.nz };
                    if (hit.hit && hit.entity != 0 && host.currentScene != nullptr)
                    {
                        result["entity"] =
                            ScriptEntity{ .entity = findEntityByUuid(*host.currentScene, hit.entity), .host = &host };
                    }
                    return result;
                })
            // Sweep a sphere (a thicker probe than raycast). Same result shape, plus a `radius`.
            .addFunction("spherecast",
                         [&host](float ox, float oy, float oz, float dx, float dy, float dz, float radius,
                                 float maxDist) -> luabridge::LuaRef
                         {
                             luabridge::LuaRef result = luabridge::newTable(host.vm.state);
                             if (!host.sphereCast)
                             {
                                 result["hit"] = false;
                                 return result;
                             }
                             const ScriptRayHit hit = host.sphereCast(ox, oy, oz, dx, dy, dz, radius, maxDist);
                             result["hit"] = hit.hit;
                             result["distance"] = hit.distance;
                             result["point"] = glm::vec3{ hit.px, hit.py, hit.pz };
                             result["normal"] = glm::vec3{ hit.nx, hit.ny, hit.nz };
                             if (hit.hit && hit.entity != 0 && host.currentScene != nullptr)
                             {
                                 result["entity"] =
                                     ScriptEntity{ .entity = findEntityByUuid(*host.currentScene, hit.entity),
                                                   .host = &host };
                             }
                             return result;
                         })
            .endNamespace();

        // Install the coroutine scheduler (se.wait/spawn_task/delay) onto the now-bound `se` table.
        if (auto installed = runString(host.vm, SchedulerPrelude, "se:scheduler"); !installed)
        {
            logError(std::format("script: scheduler prelude failed: {}", installed.error()));
        }

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
            host.currentSenderUuid = instance.entityUuid;
            auto created = callInstanceMethod(L, instance.selfRef, "on_create", std::nullopt);
            if (!created)
            {
                logError(std::format("script: on_create '{}': {}", instance.scriptPath, created.error()));
            }
        }
        host.currentSenderUuid = 0;
        flushStructuralOps(host, scene);
        dispatchMessages(host, scene);
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
            host.currentSenderUuid = instance.entityUuid;
            auto ran = callInstanceMethod(host.vm.state, instance.selfRef, "on_update", dt);
            if (!ran)
            {
                failure = ScriptRunError{ .entityUuid = instance.entityUuid,
                                          .script = instance.scriptPath,
                                          .message = std::move(ran.error()) };
                break;
            }
        }
        host.currentSenderUuid = 0;
        flushStructuralOps(host, scene);
        dispatchMessages(host, scene);
        advanceScheduler(host, dt);
        host.currentScene = nullptr;
        return failure;
    }

    auto dispatchContact(ScriptHost& host, Scene& scene, u64 entityA, u64 entityB, bool begin, bool sensor, f32 px,
                         f32 py, f32 pz, f32 nx, f32 ny, f32 nz) -> std::optional<ScriptRunError>
    {
        if (host.vm.state == nullptr || host.instances.empty())
        {
            return std::nullopt;
        }
        // v1 emits sensor enter/exit + solid Begin; a solid End has no handler.
        const char* handler = nullptr;
        bool withManifold = false;
        if (sensor)
        {
            handler = begin ? "on_trigger_enter" : "on_trigger_exit";
        }
        else if (begin)
        {
            handler = "on_contact";
            withManifold = true;
        }
        if (handler == nullptr)
        {
            return std::nullopt;
        }
        auto dispatchOne = [&](u64 selfUuid, u64 otherUuid) -> std::optional<ScriptRunError>
        {
            if (selfUuid == 0)
            {
                return std::nullopt;
            }
            const Entity other = findEntityByUuid(scene, otherUuid);
            for (const ScriptInstance& instance : host.instances)
            {
                if (instance.entityUuid != selfUuid)
                {
                    continue;
                }
                host.currentSenderUuid = instance.entityUuid;
                auto ran =
                    callContactHandler(host, instance.selfRef, handler, other, withManifold, px, py, pz, nx, ny, nz);
                if (!ran)
                {
                    return ScriptRunError{ .entityUuid = instance.entityUuid,
                                           .script = instance.scriptPath,
                                           .message = std::move(ran.error()) };
                }
            }
            return std::nullopt;
        };
        host.currentScene = &scene;
        std::optional<ScriptRunError> failure = dispatchOne(entityA, entityB);
        if (!failure)
        {
            failure = dispatchOne(entityB, entityA);
        }
        host.currentSenderUuid = 0;
        flushStructuralOps(host, scene);
        dispatchMessages(host, scene);
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
        host.pendingDestroy.clear();
        host.hierarchyDirty = false;
        host.messages.clear();  // payload refs die with the VM below
        host.currentSenderUuid = 0;
        host.currentRegistry = nullptr;
        host.input = nullptr;
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
        // number/bool/string map 1:1, an se.Vec3 userdata is a vec3 (captured as a 3-number JSON
        // array, the shape the Inspector + override storage use), anything else is not a field.
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
            if (isVec3Userdata(L, -1))
            {
                const glm::vec3 v = readVec3Userdata(L, -1);
                return ScriptField{ .name = std::move(name),
                                    .type = ScriptFieldType::Vec3,
                                    .defaultValue = nlohmann::json::array({ v.x, v.y, v.z }) };
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
