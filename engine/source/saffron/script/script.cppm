module;

// LuaBridge3 is a header-heavy C++ library, so this module uses classic
// includes (no `import std`), like the rendering/scene modules. The Lua
// headers ship without a C++ guard and must precede LuaBridge.
extern "C"
{
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <LuaBridge/LuaBridge.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module Saffron.Script;

import Saffron.Core;
import Saffron.Scene;

export namespace se
{
    /// Owns one Lua VM. Move-only; closes the lua_State in the destructor.
    struct ScriptVm
    {
        lua_State* state = nullptr;

        ScriptVm() = default;
        ScriptVm(ScriptVm&& other) noexcept;
        ScriptVm& operator=(ScriptVm&& other) noexcept;
        ScriptVm(const ScriptVm&) = delete;
        ScriptVm& operator=(const ScriptVm&) = delete;
        ~ScriptVm();
    };

    /// Create a VM with a minimal, sandboxed library set (base/coroutine/string/
    /// math/table/utf8 — no io/os/debug/package) and the `se` namespace bound.
    auto newScriptVm() -> Result<ScriptVm>;

    /// Load + run a Lua source string under the given chunk name.
    /// Syntax and runtime errors become Err carrying a traceback.
    auto runString(ScriptVm& vm, std::string_view source, std::string_view chunkName) -> Result<void>;

    /// Headless spike check: run a good chunk, a broken chunk, and a sandbox
    /// probe in a fresh VM; an Err means the runtime or bindings are broken.
    auto runScriptSelfTest() -> Result<void>;

    /// One live script instance: one slot of one entity's ScriptComponent, holding
    /// a registry ref to its `self` table. Within an entity, instances keep slot order.
    struct ScriptInstance
    {
        Entity entity{};
        u64 entityUuid = 0;
        std::string scriptPath;
        i32 slotIndex = 0;
        int selfRef = 0;
    };

    /// A contained per-instance failure from a tick, traceback included.
    struct ScriptRunError
    {
        u64 entityUuid = 0;
        std::string script;
        std::string message;
    };

    /// One queued inter-script message: a handler name dispatched to the target entity's scripts (or
    /// every script when target == 0) with the sender entity + an optional payload (a Lua registry ref,
    /// LUA_NOREF when none). Drained after each instance loop, like the structural ops.
    struct ScriptMessage
    {
        u64 target = 0;  // 0 = broadcast
        u64 sender = 0;
        std::string handler;
        int payloadRef = -2;  // LUA_NOREF
    };

    /// A rig's live ragdoll state surfaced to Lua (Jolt-free POD), filled by the Host's ragdollState bridge.
    struct ScriptRagdollState
    {
        bool present = false;
        bool active = false;
        f32 bodyWeight = 0.0f;
        i32 bones = 0;
    };

    /// A physics ray hit surfaced to Lua (Jolt-free POD). The Host fills it from raycastWorld; this
    /// keeps Saffron.Script free of a Physics edge — the binding only ever sees plain components.
    struct ScriptRayHit
    {
        bool hit = false;
        u64 entity = 0;
        f32 px = 0.0f;
        f32 py = 0.0f;
        f32 pz = 0.0f;
        f32 nx = 0.0f;
        f32 ny = 0.0f;
        f32 nz = 0.0f;
        f32 distance = 0.0f;
    };

    /// The per-entity script runtime: one VM for the whole play session, class
    /// tables cached by path, instances in deterministic creation order. The scene
    /// is borrowed only while a start/tick/stop call is on the stack; the registry
    /// (component reads) is borrowed for the session.
    struct ScriptHost
    {
        ScriptVm vm;
        std::unordered_map<std::string, int> classRefByPath;
        std::vector<ScriptInstance> instances;
        Scene* currentScene = nullptr;
        const ComponentRegistry* currentRegistry = nullptr;
        const ScriptInputState* input = nullptr;  // borrowed; the Host fills it (edges + mouse) each tick
        // Structural ops are deferred: entity:destroy() queues a uuid here and the handle stays valid for
        // the rest of the handler; the queue is drained + one relinkHierarchy runs after each instance loop.
        std::vector<u64> pendingDestroy;
        bool hierarchyDirty = false;
        // Inter-script messages queued during a tick, dispatched after the instance loop. currentSenderUuid
        // is the instance whose handler is running (the sender of an entity:send / se.broadcast).
        std::vector<ScriptMessage> messages;
        u64 currentSenderUuid = 0;
        // Physics bridges: the Host binds each to a Saffron.Physics call so Lua reaches the live world
        // without Saffron.Script importing Saffron.Physics (the raycast pattern). Unset = a safe no-op.
        std::function<ScriptRayHit(f32, f32, f32, f32, f32, f32, f32)> raycast;
        std::function<ScriptRayHit(f32, f32, f32, f32, f32, f32, f32, f32)> sphereCast;  // + radius
        std::function<void(u64, glm::vec3)> applyImpulse;
        std::function<void(u64, glm::vec3)> addForce;
        std::function<void(u64, glm::vec3)> setVelocity;
        std::function<glm::vec3(u64)> getVelocity;
        std::function<bool(u64, bool)> setRagdollEnabled;     // (rig uuid, enable) -> ok
        std::function<void(u64, bool, f32)> setRagdollBlend;  // (rig uuid, motors active, body weight)
        std::function<ScriptRagdollState(u64)> ragdollState;
        // se.log sink: the Host binds this to push the line into the edit context's script-log ring so the
        // editor can drain it, without Saffron.Script importing Saffron.SceneEdit (POD signature only).
        // Unset = se.log still writes the engine log, just not the ring.
        std::function<void(u64 senderUuid, const char* message)> logSink;
    };

    /// Create the VM and instantiate every ScriptComponent slot in the scene:
    /// each script file under srcDir returns a class table with on_update(self, dt);
    /// `self.entity` is bound to the owning entity. A slot that fails to load is a
    /// logged skip; on_create(self) runs where present. The registry backs
    /// entity:get_component snapshots. Err only when no VM could be created at all.
    auto startScripts(ScriptHost& host, Scene& scene, const ComponentRegistry& registry, std::string_view srcDir,
                      const ScriptInputState& input) -> Result<void>;

    /// Run every instance's on_update(self, dt) in instance order. The first
    /// failing instance halts the tick and is returned (pause-on-error policy);
    /// the VM and all instances survive.
    auto tickScripts(ScriptHost& host, Scene& scene, f32 dt) -> std::optional<ScriptRunError>;

    /// Dispatch a contact transition to both entities' scripts: a sensor Begin calls
    /// on_trigger_enter(self, other), a sensor End on_trigger_exit(self, other), a solid Begin
    /// on_contact(self, other, point, normal). A missing handler is a silent skip; the first
    /// failing handler is returned (pause-on-error, like tickScripts). point/normal are world
    /// space, passed as components since this interface stays glm-free.
    auto dispatchContact(ScriptHost& host, Scene& scene, u64 entityA, u64 entityB, bool begin, bool sensor, f32 px,
                         f32 py, f32 pz, f32 nx, f32 ny, f32 nz) -> std::optional<ScriptRunError>;

    /// Call on_destroy(self) where present, drop every instance + class, and close
    /// the VM. Never touches a scene — safe after the play duplicate is gone.
    void stopScripts(ScriptHost& host);

    /// The inferred edit-time type of a script-declared property: number, boolean,
    /// string, or a table of exactly 3 numbers (vec3).
    enum class ScriptFieldType : u8
    {
        Number,
        Bool,
        String,
        Vec3
    };

    auto scriptFieldTypeName(ScriptFieldType type) -> const char*;  // "number"|"bool"|"string"|"vec3"

    /// One script-declared editable field: the `properties` key, the type inferred
    /// from its default, and the default itself (vec3 as a 3-number JSON array).
    struct ScriptField
    {
        std::string name;
        ScriptFieldType type;
        nlohmann::json defaultValue;
    };

    /// Read a script's declared `properties` at edit time in a throwaway sandboxed
    /// VM — no gameplay runs; the chunk only builds tables (declaration must be
    /// side-effect-free). Entries whose type cannot be inferred are skipped with a
    /// logged note. Fields come back sorted by name. Err on a load/run failure.
    auto readScriptSchema(std::string_view path) -> Result<std::vector<ScriptField>>;
}

namespace se
{
    ScriptVm::ScriptVm(ScriptVm&& other) noexcept : state(std::exchange(other.state, nullptr)) {}

    ScriptVm& ScriptVm::operator=(ScriptVm&& other) noexcept
    {
        if (this != &other)
        {
            if (state != nullptr)
            {
                lua_close(state);
            }
            state = std::exchange(other.state, nullptr);
        }
        return *this;
    }

    ScriptVm::~ScriptVm()
    {
        if (state != nullptr)
        {
            lua_close(state);
        }
    }

    namespace
    {
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

        // Runs the chunk sitting on top of the handler at msghIndex and maps any
        // failure to Err(traceback), leaving the stack balanced either way.
        auto finishRun(lua_State* L, int msghIndex, int loadStatus) -> Result<void>
        {
            int status = loadStatus;
            if (status == LUA_OK)
            {
                status = lua_pcall(L, 0, 0, msghIndex);
            }
            if (status != LUA_OK)
            {
                std::string error;
                const char* message = lua_tostring(L, -1);
                if (message != nullptr)
                {
                    error = message;
                }
                else
                {
                    error = std::format("script error (status {})", status);
                }
                lua_pop(L, 2);
                return Err(std::move(error));
            }
            lua_pop(L, 1);
            return {};
        }
    }

    // Defined in script_runtime.cpp (module-internal): the pure se.Vec3 value type + math helpers,
    // bound into both this schema VM and the runtime VM so a properties default of se.vec3(...) resolves.
    void registerScriptValueTypes(lua_State* L);

    auto newScriptVm() -> Result<ScriptVm>
    {
        lua_State* L = luaL_newstate();
        if (L == nullptr)
        {
            return Err("lua: out of memory creating VM");
        }
        luaL_openselectedlibs(L, LUA_GLIBK | LUA_COLIBK | LUA_STRLIBK | LUA_MATHLIBK | LUA_TABLIBK | LUA_UTF8LIBK, 0);
        luabridge::getGlobalNamespace(L)
            .beginNamespace("se")
            .addFunction(
                "log", +[](const char* message) { logInfo(message); })
            .endNamespace();
        registerScriptValueTypes(L);
        ScriptVm vm;
        vm.state = L;
        return vm;
    }

    auto runString(ScriptVm& vm, std::string_view source, std::string_view chunkName) -> Result<void>
    {
        lua_State* L = vm.state;
        lua_pushcfunction(L, tracebackHandler);
        const int msghIndex = lua_gettop(L);
        const std::string chunk(chunkName);
        return finishRun(L, msghIndex, luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t"));
    }

    auto runScriptSelfTest() -> Result<void>
    {
        auto vm = newScriptVm();
        if (!vm)
        {
            return Err(std::format("script self-test: VM creation failed: {}", vm.error()));
        }
        auto good = runString(*vm, "se.log('script self-test: binding ok'); assert(1 + 1 == 2)", "selftest");
        if (!good)
        {
            return Err(std::format("script self-test: good chunk failed: {}", good.error()));
        }
        auto broken = runString(*vm, "error('deliberate')", "selftest-broken");
        if (broken)
        {
            return Err("script self-test: broken chunk unexpectedly succeeded");
        }
        if (broken.error().find("deliberate") == std::string::npos ||
            broken.error().find("stack traceback") == std::string::npos)
        {
            return Err(std::format("script self-test: error lacks a traceback: {}", broken.error()));
        }
        auto sandbox =
            runString(*vm, "assert(io == nil and os == nil and debug == nil and package == nil)", "selftest-sandbox");
        if (!sandbox)
        {
            return Err(std::format("script self-test: sandbox probe failed: {}", sandbox.error()));
        }
        return {};
    }
}
