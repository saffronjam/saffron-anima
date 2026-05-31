module;

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <format>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

module Saffron.Control;

import Saffron.Core;
import Saffron.Json;
import Saffron.Window;
import Saffron.Rendering;
import Saffron.Scene;
import Saffron.Editor;
import Saffron.Assets;

namespace se
{
    void registerCommand(CommandRegistry& reg, std::string name, std::string help,
                         std::function<Result<json>(EngineContext&, const json&)> run)
    {
        const std::size_t index = reg.rows.size();
        reg.byName[name] = index;
        reg.rows.push_back(CommandTraits{ std::move(name), std::move(help), std::move(run) });
    }

    auto findCommand(const CommandRegistry& reg, const std::string& name) -> const CommandTraits*
    {
        auto it = reg.byName.find(name);
        if (it == reg.byName.end())
        {
            return nullptr;
        }
        return &reg.rows[it->second];
    }

    auto positionalOr(const json& params, const std::string& name, std::size_t index) -> json
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

    auto asString(const json& value, std::string fallback) -> std::string
    {
        if (value.is_string())
        {
            return value.get<std::string>();
        }
        return fallback;
    }

    auto resolveEntity(EngineContext& ctx, const json& params) -> Result<Entity>
    {
        const json selector = positionalOr(params, "entity", 0);
        if (selector.is_null())
        {
            return Err(std::string{ "missing 'entity' (uuid or name)" });
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
            return Err(std::format("entity not found: {}", dumpJson(selector)));
        }
        return found;
    }

    auto entityRef(Scene& scene, Entity entity) -> json
    {
        return json{ { "id", getComponent<IdComponent>(scene, entity).id.value },
                     { "name", getComponent<NameComponent>(scene, entity).name } };
    }

    void registerBuiltinCommands(CommandRegistry& reg)
    {
        registerRenderCommands(reg);
        registerSceneCommands(reg);
        registerAssetCommands(reg);
    }

    auto controlSocketPath() -> std::string
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

    auto startControlServer(std::string path) -> Result<ControlServer>
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
        {
            return Err(std::format("socket: {}", std::strerror(errno)));
        }
        ::unlink(path.c_str());

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path))
        {
            ::close(fd);
            return Err(std::format("socket path too long: {}", path));
        }
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            return Err(std::format("bind '{}': {}", path, std::strerror(errno)));
        }
        // Owner-only file permission is the access control (with the 0700 runtime dir).
        ::chmod(path.c_str(), 0600);
        if (::listen(fd, 8) != 0)
        {
            ::close(fd);
            return Err(std::format("listen: {}", std::strerror(errno)));
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

    auto dispatch(CommandRegistry& reg, EngineContext& ctx, const json& request) -> json
    {
        json reply;
        reply["id"] = request.value("id", json{});
        const std::string command = jsonStringOr(request, "cmd", std::string{});
        const CommandTraits* row = findCommand(reg, command);
        if (row == nullptr)
        {
            reply["ok"] = false;
            reply["error"] = std::format("unknown command '{}'", command);
            return reply;
        }
        const json params = request.value("params", json::object());
        auto result = row->run(ctx, params);
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

                auto request = parseJson(line);
                json reply;
                if (!request)
                {
                    reply = json{ { "ok", false }, { "error", "invalid JSON request" } };
                }
                else
                {
                    reply = dispatch(reg, ctx, *request);
                }

                std::string out = dumpJson(reply);
                out.push_back('\n');
                static_cast<void>(::send(client.fd, out.data(), out.size(), MSG_NOSIGNAL));
            }
        }

        std::erase_if(server.clients, [](const ControlClient& client) -> bool { return client.fd < 0; });
    }

    auto newControlContext() -> ControlContext*
    {
        ControlContext* ctx = new ControlContext();
        registerBuiltinCommands(ctx->registry);

        auto server = startControlServer(controlSocketPath());
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
