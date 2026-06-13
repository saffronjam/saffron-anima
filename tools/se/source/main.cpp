#include "../args.hxx"
#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    using json = nlohmann::json;

    enum class OutputMode
    {
        Text,
        Json
    };

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
        if (token == "true")
        {
            return true;
        }
        if (token == "false")
        {
            return false;
        }
        if (token == "null")
        {
            return nullptr;
        }
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
        for (std::size_t i = 0; i < args.size();)
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
                std::printf("  %-22s  %s\n", entry.value("name", "").c_str(), entry.value("help", "").c_str());
            }
            return;
        }
        if (cmd == "ping")
        {
            std::printf("pong  engine=%s  version=%s  pid=%d\n", result.value("engine", "").c_str(),
                        result.value("version", "").c_str(), result.value("pid", 0));
            return;
        }
        if (cmd == "list-entities" && result.contains("entities"))
        {
            for (const auto& e : result["entities"])
            {
                std::printf("  %-24s  %-24s  %s\n", e.value("id", std::string{}).c_str(), e.value("name", "").c_str(),
                            e.value("parentId", std::string{}).c_str());
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
                std::printf("  %-8s  %-32s  %s\n", a.value("type", "").c_str(), a.value("name", "").c_str(),
                            a.value("id", std::string{}).c_str());
            }
            return;
        }
        if (cmd == "get-asset-model")
        {
            std::printf("model %s  (mesh %s)\n", result.value("name", "").c_str(),
                        result.value("mesh", std::string{}).c_str());
            const auto caps = result.value("capabilities", json::object());
            std::printf("  meshes=%d  materials=%d  nodes=%d  rig=%s  bones=%d  clips=%d\n",
                        caps.value("meshCount", 0), caps.value("materialCount", 0), caps.value("nodeCount", 0),
                        caps.value("hasRig", false) ? "yes" : "no", caps.value("boneCount", 0),
                        caps.value("clipCount", 0));
            const auto& bones = result.value("bones", json::array());
            for (const auto& b : bones)
            {
                int depth = 0;
                int parent = b.value("parent", -1);
                for (int guard = 0; parent >= 0 && parent < static_cast<int>(bones.size()) && guard < 256; guard += 1)
                {
                    depth += 1;
                    parent = bones[static_cast<std::size_t>(parent)].value("parent", -1);
                }
                std::printf("  %*s%s%s\n", depth * 2, "", b.value("name", "").c_str(),
                            b.value("joint", false) ? "  [joint]" : "");
            }
            for (const auto& c : result.value("clips", json::array()))
            {
                std::printf("  clip %-28s  %8.3fs  %s\n", c.value("name", "").c_str(), c.value("duration", 0.0),
                            c.value("id", std::string{}).c_str());
            }
            return;
        }
        if (cmd == "list-clips" && result.contains("clips"))
        {
            for (const auto& c : result["clips"])
            {
                std::printf("  %-32s  %8.3fs  %s\n", c.value("name", "").c_str(), c.value("duration", 0.0),
                            c.value("id", std::string{}).c_str());
            }
            return;
        }
        if (cmd == "render-stats")
        {
            std::printf("cpu=%.2fms  gpu=%.2fms  wait=%.2fms  fps=%.0f  draws=%d  tris=%d  binds=%d  pso+=%d%s\n",
                        result.value("cpuFrameMs", 0.0), result.value("gpuFrameMs", 0.0),
                        result.value("cpuWaitMs", 0.0), result.value("fps", 0.0), result.value("drawCalls", 0),
                        result.value("triangles", 0), result.value("descriptorBinds", 0),
                        result.value("pipelinesCreated", 0),
                        result.value("softwareGpu", false) ? "  [software-gpu]" : "");
            return;
        }
        if (cmd == "profiler.set-mode")
        {
            std::printf("mode=%s  timestamps=%s  pipeline-stats=%s%s\n", result.value("mode", "").c_str(),
                        result.value("timestampsSupported", false) ? "yes" : "no",
                        result.value("pipelineStatsSupported", false) ? "yes" : "no",
                        result.value("softwareGpu", false) ? "  [software-gpu]" : "");
            return;
        }
        if (cmd == "pass-timings")
        {
            if (result.value("softwareGpu", false))
            {
                std::printf("[software-gpu: timings are CPU rasterization time, not hardware]\n");
            }
            for (const auto& p : result.value("passes", json::array()))
            {
                std::printf("  %-28s  %8.3f ms\n", p.value("name", "").c_str(), p.value("gpuMs", 0.0));
            }
            std::printf("  %-28s  %8.3f ms\n", "total (span)", result.value("gpuTotalMs", 0.0));
            return;
        }
        if (cmd == "profiler.capture-start")
        {
            std::printf("armed capture id=%llu  (stop with: se profiler.capture-stop)\n",
                        static_cast<unsigned long long>(result.value("captureId", 0ULL)));
            return;
        }
        if (cmd == "profiler.capture-stop")
        {
            if (!result.value("ready", false))
            {
                std::printf("no capture ready (arm one with: se profiler.capture-start)\n");
                return;
            }
            const auto meta = result.value("capture", json::object()).value("metadata", json::object());
            std::printf("captured %u frame(s), %zu spans  [%s%s]\n", result.value("frameCount", 0u),
                        result.value("capture", json::object()).value("spans", json::array()).size(),
                        meta.value("correlated", false) ? "correlated" : "uncorrelated",
                        meta.value("softwareGpu", false) ? ", software-gpu" : "");
            std::string path = result.value("path", std::string{});
            if (path.empty())
            {
                // Inline capture: write the Chrome-Trace JSON to a temp file the user can open.
                const std::string trace = result.value("chromeTrace", std::string{});
                if (!trace.empty())
                {
                    path = (std::filesystem::temp_directory_path() / "saffron-profile.json").string();
                    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                    stream << trace;
                }
            }
            if (!path.empty())
            {
                std::printf("trace: %s  (open in chrome://tracing or ui.perfetto.dev)\n", path.c_str());
            }
            return;
        }
        if (cmd == "frame-history")
        {
            std::printf(
                "p50=%.2f  p95=%.2f  p99=%.2f  p99.9=%.2f  max=%.2f  stddev=%.2f  budget=%.2fms  stutters=%lld  n=%d\n",
                result.value("p50Ms", 0.0), result.value("p95Ms", 0.0), result.value("p99Ms", 0.0),
                result.value("p999Ms", 0.0), result.value("maxMs", 0.0), result.value("stddevMs", 0.0),
                result.value("budgetMs", 0.0), result.value("stutterCount", 0LL), result.value("sampleCount", 0));
            return;
        }
        if (cmd == "get-perf-config" || cmd == "set-perf-config")
        {
            std::printf("targetFps=%.0f  budget=%.2fms  green<%.2f×budget  amber<%.1f×median  frozen=%.0fms  "
                        "vram warn/crit=%.0f%%/%.0f%%\n",
                        result.value("targetFps", 0.0), result.value("budgetMs", 0.0),
                        result.value("greenBudgetFrac", 0.0), result.value("amberMedianMul", 0.0),
                        result.value("frozenMs", 0.0), result.value("vramWarnFrac", 0.0) * 100.0,
                        result.value("vramCritFrac", 0.0) * 100.0);
            return;
        }
        if (cmd == "drain-alarms")
        {
            const auto events = result.value("events", json::array());
            for (const auto& e : events)
            {
                std::printf("  #%-5lld  %-8s %-9s %-13s  %8.2f / %-8.2f  x%d\n", e.value("seq", 0LL),
                            e.value("state", "").c_str(), e.value("severity", "").c_str(),
                            e.value("metric", "").c_str(), e.value("value", 0.0), e.value("threshold", 0.0),
                            e.value("count", 0));
            }
            std::printf("  high=%lld  oldest=%lld  overflowed=%s  (%zu events)\n", result.value("highWaterSeq", 0LL),
                        result.value("oldestSeq", 0LL), result.value("overflowed", false) ? "yes" : "no",
                        events.size());
            return;
        }
        if (cmd == "list-active-alarms")
        {
            const auto alarms = result.value("alarms", json::array());
            if (alarms.empty())
            {
                std::printf("no active alarms\n");
                return;
            }
            for (const auto& a : alarms)
            {
                const std::string pass = a.value("pass", std::string{});
                std::printf("  %-9s %-13s  %8.2f / %-8.2f  x%d%s%s\n", a.value("severity", "").c_str(),
                            a.value("metric", "").c_str(), a.value("value", 0.0), a.value("threshold", 0.0),
                            a.value("count", 0), pass.empty() ? "" : "  pass=", pass.c_str());
            }
            return;
        }
        if (cmd == "play" || cmd == "pause" || cmd == "stop" || cmd == "step" || cmd == "get-play-state")
        {
            std::printf("state=%s  playVersion=%d  sceneVersion=%d  camera=%s\n", result.value("state", "").c_str(),
                        result.value("playVersion", 0), result.value("sceneVersion", 0),
                        result.value("hasPrimaryCamera", false) ? "ok" : "missing");
            return;
        }
        if (cmd == "viewport-native-info")
        {
            std::printf("%s  %s  %ux%u  sock=%s\n", result.value("status", "").c_str(),
                        result.value("transport", "").c_str(), result.value("width", 0u), result.value("height", 0u),
                        result.value("controlSocket", "").c_str());
            return;
        }
        if (cmd == "set-active-view")
        {
            std::printf("view=%s\n", result.value("view", "").c_str());
            return;
        }
        if (cmd == "get-selection")
        {
            if (result.contains("entity") && result["entity"].is_object())
            {
                std::printf("selected: %s  (sel v%llu, scene v%llu)\n", result["entity"].value("name", "").c_str(),
                            result.value("selectionVersion", 0ULL), result.value("sceneVersion", 0ULL));
            }
            else
            {
                std::printf("no selection  (sel v%llu, scene v%llu)\n", result.value("selectionVersion", 0ULL),
                            result.value("sceneVersion", 0ULL));
            }
            return;
        }
        if (cmd == "add-entity" || cmd == "copy-entity")
        {
            std::printf("%s  id=%s\n", result.value("name", "").c_str(), result.value("id", std::string{}).c_str());
            return;
        }
        if (cmd == "get-gizmo" || cmd == "set-gizmo")
        {
            std::printf("op=%s  space=%s\n", result.value("op", "").c_str(), result.value("space", "").c_str());
            return;
        }
        if (cmd == "gizmo-pointer")
        {
            std::printf("hovered=%s  dragging=%s\n", result.value("hovered", "none").c_str(),
                        result.value("dragging", false) ? "yes" : "no");
            return;
        }
        if (cmd == "pick")
        {
            if (result.value("hit", false))
            {
                std::printf("%s  %s  id=%s\n", result.value("kind", "").c_str(), result.value("name", "").c_str(),
                            result.value("id", std::string{}).c_str());
            }
            else
            {
                std::printf("no hit\n");
            }
            return;
        }
        if (cmd == "pick-skeleton-joint")
        {
            if (result.value("found", false))
            {
                std::printf("joint node=%d\n", result.value("nodeIndex", -1));
            }
            else
            {
                std::printf("no joint\n");
            }
            return;
        }
        if (cmd == "get-camera" || cmd == "set-camera")
        {
            const json p = result.value("position", json::object());
            std::printf("pos=(%.2f, %.2f, %.2f)  yaw=%.1f  pitch=%.1f  fov=%.1f\n", p.value("x", 0.0),
                        p.value("y", 0.0), p.value("z", 0.0), result.value("yaw", 0.0), result.value("pitch", 0.0),
                        result.value("fov", 0.0));
            return;
        }
        if (cmd == "get-thumbnail" || cmd == "view-asset")
        {
            const std::string b64 = result.value("base64", std::string{});
            std::printf("%s %ux%u  ~%zu bytes (base64 %zu chars)\n", result.value("format", "").c_str(),
                        result.value("width", 0u), result.value("height", 0u), (b64.size() / 4) * 3, b64.size());
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
        for (int i = 1; i < argc;)
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
    args::ArgumentParser parser("se — SaffronEngine control CLI");
    parser.Prog("se");
    args::HelpFlag help(parser, "help", "show this help", { 'h', "help" });
    args::MapFlag<std::string, OutputMode> output(parser, "output", "output format", { 'o', "output" },
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

    const std::string& cmd = split.cmd;
    const OutputMode mode = args::get(output);

    json request;
    request["cmd"] = cmd;
    request["params"] = buildParams(split.engArgs);
    request["id"] = 1;

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
