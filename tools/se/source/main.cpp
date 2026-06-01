#include "../args.hxx"
#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using json = nlohmann::json;

    enum class OutputMode { Text, Json };

    std::string socketPath()
    {
        if (const char* override = std::getenv("SAFFRON_CONTROL_SOCK"))
        {
            return override;
        }
        if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"))
        {
            return std::string(runtime) + "/saffron-control.sock";
        }
        return "/tmp/saffron-control-" + std::to_string(static_cast<unsigned>(getuid())) + ".sock";
    }

    // true/false/null, integer, float, explicit json ({ [ "), else a bare string.
    json coerce(const std::string& token)
    {
        if (token == "true") { return true; }
        if (token == "false") { return false; }
        if (token == "null") { return nullptr; }
        if (!token.empty() && (token.front() == '{' || token.front() == '[' || token.front() == '"'))
        {
            json value = json::parse(token, nullptr, false);
            if (!value.is_discarded())
            {
                return value;
            }
        }
        char* end = nullptr;
        if (!token.empty() && token.front() != '-')
        {
            const unsigned long long asUnsigned = std::strtoull(token.c_str(), &end, 10);
            if (end != token.c_str() && *end == '\0')
            {
                return asUnsigned;
            }
        }
        end = nullptr;
        const long long asInt = std::strtoll(token.c_str(), &end, 10);
        if (end != token.c_str() && *end == '\0')
        {
            return asInt;
        }
        const double asDouble = std::strtod(token.c_str(), &end);
        if (end != token.c_str() && *end == '\0')
        {
            return asDouble;
        }
        return token;
    }

    // Positionals go to params["args"]; --flag value / --flag=value / bare --flag map
    // to params[flag]. Commands read positionals or flags via the same key.
    json buildParams(const std::vector<std::string>& args)
    {
        json params = json::object();
        json positional = json::array();
        for (std::size_t i = 0; i < args.size(); )
        {
            const std::string& arg = args[i];
            if (arg.rfind("--", 0) == 0)
            {
                std::string key = arg.substr(2);
                const std::size_t eq = key.find('=');
                if (eq != std::string::npos)
                {
                    params[key.substr(0, eq)] = coerce(key.substr(eq + 1));
                    i += 1;
                }
                else if (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0)
                {
                    params[key] = coerce(args[i + 1]);
                    i += 2;
                }
                else
                {
                    params[key] = true;
                    i += 1;
                }
            }
            else
            {
                positional.push_back(coerce(arg));
                i += 1;
            }
        }
        if (!positional.empty())
        {
            params["args"] = positional;
        }
        return params;
    }

    void printResult(const std::string& cmd, const json& result, OutputMode mode)
    {
        if (mode == OutputMode::Json)
        {
            std::printf("%s\n", result.dump(2).c_str());
            return;
        }
        if (cmd == "help" && result.contains("commands"))
        {
            for (const auto& entry : result["commands"])
            {
                std::printf("  %-22s  %s\n",
                    entry.value("name", "").c_str(),
                    entry.value("help", "").c_str());
            }
            return;
        }
        if (cmd == "ping")
        {
            std::printf("pong  engine=%s  version=%s  pid=%d\n",
                result.value("engine", "").c_str(),
                result.value("version", "").c_str(),
                result.value("pid", 0));
            return;
        }
        if (cmd == "list-entities" && result.contains("entities"))
        {
            for (const auto& e : result["entities"])
            {
                std::printf("  %-24s  %s\n",
                    std::to_string(e.value("id", std::uint64_t{ 0 })).c_str(),
                    e.value("name", "").c_str());
            }
            return;
        }
        if (cmd == "list-components" && result.contains("components"))
        {
            for (const auto& c : result["components"])
            {
                std::printf("  %s\n", c.get<std::string>().c_str());
            }
            return;
        }
        if (cmd == "list-assets" && result.contains("assets"))
        {
            for (const auto& a : result["assets"])
            {
                std::printf("  %-8s  %-32s  %s\n",
                    a.value("type", "").c_str(),
                    a.value("name", "").c_str(),
                    std::to_string(a.value("id", std::uint64_t{ 0 })).c_str());
            }
            return;
        }
        if (cmd == "render-stats")
        {
            std::printf("draws=%-4d  batches=%-4d  instances=%-4d  clustered=%s\n",
                result.value("drawCalls", 0),
                result.value("batches", 0),
                result.value("instances", 0),
                result.value("clustered", false) ? "on" : "off");
            return;
        }
        if (cmd == "viewport-native-info")
        {
            std::printf("%s  %s  %ux%u  sock=%s\n",
                result.value("status", "").c_str(), result.value("transport", "").c_str(),
                result.value("width", 0u), result.value("height", 0u),
                result.value("controlSocket", "").c_str());
            return;
        }
        if (cmd == "attach-native-viewport" || cmd == "resize-native-viewport")
        {
            std::printf("%s  %dx%d @ (%d,%d)\n",
                (result.value("attached", false) || result.value("resized", false)) ? "ok" : "fail",
                result.value("width", 0), result.value("height", 0),
                result.value("x", 0), result.value("y", 0));
            return;
        }
        if (cmd == "get-selection")
        {
            if (result.contains("entity") && result["entity"].is_object())
            {
                std::printf("selected: %s  (sel v%llu, scene v%llu)\n",
                    result["entity"].value("name", "").c_str(),
                    result.value("selectionVersion", 0ULL), result.value("sceneVersion", 0ULL));
            }
            else
            {
                std::printf("no selection  (sel v%llu, scene v%llu)\n",
                    result.value("selectionVersion", 0ULL), result.value("sceneVersion", 0ULL));
            }
            return;
        }
        if (cmd == "add-entity" || cmd == "copy-entity")
        {
            std::printf("%s  id=%s\n", result.value("name", "").c_str(),
                std::to_string(result.value("id", std::uint64_t{ 0 })).c_str());
            return;
        }
        if (cmd == "get-gizmo" || cmd == "set-gizmo")
        {
            std::printf("op=%s  space=%s\n",
                result.value("op", "").c_str(), result.value("space", "").c_str());
            return;
        }
        if (cmd == "gizmo-pointer")
        {
            std::printf("hovered=%s  dragging=%s\n",
                result.value("hovered", "none").c_str(),
                result.value("dragging", false) ? "yes" : "no");
            return;
        }
        if (cmd == "pick")
        {
            if (result.value("hit", false))
            {
                std::printf("%s  %s  id=%s\n",
                    result.value("kind", "").c_str(),
                    result.value("name", "").c_str(),
                    std::to_string(result.value("id", std::uint64_t{ 0 })).c_str());
            }
            else
            {
                std::printf("no hit\n");
            }
            return;
        }
        if (cmd == "get-camera" || cmd == "set-camera")
        {
            const json p = result.value("position", json::object());
            std::printf("pos=(%.2f, %.2f, %.2f)  yaw=%.1f  pitch=%.1f  fov=%.1f\n",
                p.value("x", 0.0), p.value("y", 0.0), p.value("z", 0.0),
                result.value("yaw", 0.0), result.value("pitch", 0.0), result.value("fov", 0.0));
            return;
        }
        if (cmd == "get-thumbnail" || cmd == "view-asset")
        {
            const std::string b64 = result.value("base64", std::string{});
            std::printf("%s %ux%u  ~%zu bytes (base64 %zu chars)\n",
                result.value("format", "").c_str(),
                result.value("width", 0u), result.value("height", 0u),
                (b64.size() / 4) * 3, b64.size());
            return;
        }
        // Fallback: pretty JSON with UTF-8 unescaped (so — renders as — rather than —).
        std::printf("%s\n", result.dump(2, ' ', false).c_str());
    }

    // Split argv into se-level flags, the command, and engine args.
    // Se-level flags are known at compile time and stripped before forwarding to the engine.
    // Adding a new se flag: add extraction here + a matching declaration in the args parser below.
    struct SplitArgs
    {
        std::vector<std::string> seFlags;  // tokens for the args parser
        std::string cmd;
        std::vector<std::string> engArgs;
    };

    SplitArgs splitArgs(int argc, char** argv)
    {
        SplitArgs r;
        for (int i = 1; i < argc; )
        {
            std::string arg = argv[i];
            if ((arg == "-o" || arg == "--output") && i + 1 < argc)
            {
                r.seFlags.push_back(arg);
                r.seFlags.push_back(argv[++i]);
                i++;
            }
            else if (arg.rfind("--output=", 0) == 0 || arg == "-h" || arg == "--help")
            {
                r.seFlags.push_back(arg);
                i++;
            }
            else if (r.cmd.empty() && (arg.empty() || arg[0] != '-'))
            {
                r.cmd = arg;
                i++;
            }
            else
            {
                r.engArgs.push_back(arg);
                i++;
            }
        }
        return r;
    }
}

int main(int argc, char** argv)
{
    args::ArgumentParser parser("se — SaffronEditor control CLI");
    parser.Prog("se");
    args::HelpFlag help(parser, "help", "show this help", {'h', "help"});
    args::MapFlag<std::string, OutputMode> output(parser, "output",
        "output format",
        {'o', "output"},
        { { "text", OutputMode::Text }, { "json", OutputMode::Json } },
        OutputMode::Text);

    const auto split = splitArgs(argc, argv);

    try
    {
        parser.ParseArgs(split.seFlags);
    }
    catch (const args::Help&)
    {
        std::cout << parser;
        return 0;
    }
    catch (const args::ParseError& e)
    {
        std::fprintf(stderr, "se: %s\n", e.what());
        std::fprintf(stderr, "%s", parser.Help().c_str());
        return 1;
    }

    if (args::get(help))
    {
        std::cout << parser;
        return 0;
    }

    if (split.cmd.empty())
    {
        std::fprintf(stderr, "se: missing command\n%s", parser.Help().c_str());
        return 2;
    }

    const std::string& cmd  = split.cmd;
    const OutputMode   mode = args::get(output);

    json request;
    request["cmd"]    = cmd;
    request["params"] = buildParams(split.engArgs);
    request["id"]     = 1;

    const std::string path = socketPath();
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        std::perror("se: socket");
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        std::fprintf(stderr, "se: cannot connect to %s: %s\n", path.c_str(), std::strerror(errno));
        ::close(fd);
        return 1;
    }

    std::string line = request.dump();
    line.push_back('\n');
    static_cast<void>(::send(fd, line.data(), line.size(), MSG_NOSIGNAL));

    std::string reply;
    char buffer[4096];
    while (reply.find('\n') == std::string::npos)
    {
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0)
        {
            break;
        }
        reply.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(fd);

    json response = json::parse(reply, nullptr, false);
    if (response.is_discarded())
    {
        std::fprintf(stderr, "se: malformed reply\n");
        return 1;
    }
    if (response.value("ok", false))
    {
        printResult(cmd, response.value("result", json::object()), mode);
        return 0;
    }
    std::fprintf(stderr, "se: %s\n", response.value("error", std::string{ "error" }).c_str());
    return 1;
}
