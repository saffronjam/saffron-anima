module;

// The control plane's command registry + socket types. nlohmann/json is header-heavy,
// so this partition uses classic includes (no `import std`), like the rest of control.
#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

export module Saffron.Control:Command;

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
        std::function<Result<json>(EngineContext&, const json&)> run;
    };

    struct CommandRegistry
    {
        std::vector<CommandTraits> rows;
        std::unordered_map<std::string, std::size_t> byName;
    };

    void registerCommand(CommandRegistry& reg, std::string name, std::string help,
                         std::function<Result<json>(EngineContext&, const json&)> run);
    auto findCommand(const CommandRegistry& reg, const std::string& name) -> const CommandTraits*;

    // params[name] if present, else the index-th element of params["args"], else null.
    // Lets every command accept either `--name value` or a bare positional.
    auto positionalOr(const json& params, const std::string& name, std::size_t index) -> json;
    auto asString(const json& value, std::string fallback) -> std::string;
    auto resolveEntity(EngineContext& ctx, const json& params) -> Result<Entity>;
    auto entityRef(Scene& scene, Entity entity) -> json;

    // The built-in commands, grouped by concern. Registered in render → scene → asset
    // order (help/list iterate the registry in insertion order).
    void registerRenderCommands(CommandRegistry& reg);
    void registerSceneCommands(CommandRegistry& reg);
    void registerAssetCommands(CommandRegistry& reg);
    void registerBuiltinCommands(CommandRegistry& reg);

    auto controlSocketPath() -> std::string;

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

    auto startControlServer(std::string path) -> Result<ControlServer>;
    void stopControlServer(ControlServer& server);
    auto dispatch(CommandRegistry& reg, EngineContext& ctx, const json& request) -> json;
    // Accepts pending clients, splits newline-delimited requests, and runs each on
    // the calling thread. Never blocks: recv with MSG_DONTWAIT, replies are single
    // compact json lines, send uses MSG_NOSIGNAL so a vanished client cannot signal.
    void drainControlServer(ControlServer& server, CommandRegistry& reg, EngineContext& ctx);

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
    auto newControlContext() -> ControlContext*;
    void destroyControlContext(ControlContext* ctx);

    /// Drains and runs any pending control commands on the calling (main) thread.
    /// Call once per frame.
    void pollControl(ControlContext& ctx, Window& window, Renderer& renderer, EditorContext& editor, AssetServer& assets);
}
