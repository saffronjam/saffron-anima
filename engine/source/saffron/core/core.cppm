export module Saffron.Core;

import std;

namespace sa
{
    // Maps a caller's source path to its module directory under source/saffron/
    // ("rendering", "scene", …) — the default subsystem tag for the log functions.
    auto logSubsystem(std::source_location location) -> std::string_view
    {
        std::string_view path = location.file_name();
        constexpr std::string_view root = "source/saffron/";
        const std::size_t rootAt = path.rfind(root);
        if (rootAt == std::string_view::npos)
        {
            return "engine";
        }
        path.remove_prefix(rootAt + root.size());
        const std::size_t slash = path.find('/');
        if (slash == std::string_view::npos)
        {
            return "engine";
        }
        return path.substr(0, slash);
    }
}

export namespace sa
{
    // Fixed-width aliases — short, Go-like spellings.
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;
    using i8 = std::int8_t;
    using i16 = std::int16_t;
    using i32 = std::int32_t;
    using i64 = std::int64_t;
    using f32 = float;
    using f64 = double;

    // A shared reference to a logical resource object (the meta-layer wrappers are
    // passed around as Ref<T> rather than opaque handles). Plain alias, no base class.
    template <typename T>
    using Ref = std::shared_ptr<T>;

    // Rust-style fallible return: every fallible function returns Result<T> and reports
    // failure with Err("message"). Result<T> is exactly std::expected<T, std::string>;
    // success is the value itself (or {} for Result<void>) — no Ok wrapper.
    template <typename T>
    using Result = std::expected<T, std::string>;

    inline auto Err(std::string message) -> std::unexpected<std::string>
    {
        return std::unexpected<std::string>(std::move(message));
    }

    inline constexpr std::string_view EngineName = "Saffron Anima";
    inline constexpr std::string_view EngineVersion = "0.1.0-vulkan";

    // A span of time, in seconds. Plain data; transform with free functions.
    struct TimeSpan
    {
        f32 seconds = 0.0f;
    };

    constexpr auto toMilliseconds(TimeSpan span) -> f32
    {
        return span.seconds * 1000.0f;
    }

    // A stable 64-bit identity. entt::entity values are not stable across runs,
    // so anything serialized carries a Uuid instead.
    struct Uuid
    {
        u64 value = 0;
    };

    auto newUuid() -> Uuid
    {
        static std::mt19937_64 engine{ std::random_device{}() };
        // Reserve the low range (< 1024) for built-in / synthetic assets (e.g. the default material),
        // so a minted id never collides with a reserved one.
        static std::uniform_int_distribution<u64> distribution{ 1024, std::numeric_limits<u64>::max() };
        return Uuid{ distribution(engine) };
    }

    // Standard table-based base64 (RFC 4648) of a byte buffer. Used to ship small binary
    // blobs (e.g. thumbnail PNGs) over the JSON control protocol.
    auto base64Encode(const std::vector<u8>& bytes) -> std::string
    {
        static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((bytes.size() + 2) / 3) * 4);
        std::size_t i = 0;
        for (; i + 3 <= bytes.size(); i += 3)
        {
            const u32 n = (u32(bytes[i]) << 16) | (u32(bytes[i + 1]) << 8) | u32(bytes[i + 2]);
            out.push_back(table[(n >> 18) & 0x3F]);
            out.push_back(table[(n >> 12) & 0x3F]);
            out.push_back(table[(n >> 6) & 0x3F]);
            out.push_back(table[n & 0x3F]);
        }
        if (const std::size_t rem = bytes.size() - i; rem == 1)
        {
            const u32 n = u32(bytes[i]) << 16;
            out.push_back(table[(n >> 18) & 0x3F]);
            out.push_back(table[(n >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
        else if (rem == 2)
        {
            const u32 n = (u32(bytes[i]) << 16) | (u32(bytes[i + 1]) << 8);
            out.push_back(table[(n >> 18) & 0x3F]);
            out.push_back(table[(n >> 12) & 0x3F]);
            out.push_back(table[(n >> 6) & 0x3F]);
            out.push_back('=');
        }
        return out;
    }

    /// Severity of a log line; Warn and Error insert their level before the message.
    enum class LogLevel
    {
        Info,
        Warn,
        Error,
    };

    /// Prints one stdout line `[saffron:subsystem] message` (`warn:` / `error:` ahead of
    /// the message for the non-info levels) — the prefix keeps engine output grep-able.
    /// Callers reporting on another component's behalf (e.g. the Vulkan debug messenger)
    /// pass the subsystem explicitly; everything else uses the wrappers below.
    void log(LogLevel level, std::string_view subsystem, std::string_view message)
    {
        switch (level)
        {
        case LogLevel::Info:
            std::println("[saffron:{}] {}", subsystem, message);
            return;
        case LogLevel::Warn:
            std::println("[saffron:{}] warn: {}", subsystem, message);
            return;
        case LogLevel::Error:
            std::println("[saffron:{}] error: {}", subsystem, message);
            return;
        }
    }

    /// Logs at info, tagged with the calling module's subsystem.
    void logInfo(std::string_view message, std::source_location location = std::source_location::current())
    {
        log(LogLevel::Info, logSubsystem(location), message);
    }

    /// Logs at warn, tagged with the calling module's subsystem.
    void logWarn(std::string_view message, std::source_location location = std::source_location::current())
    {
        log(LogLevel::Warn, logSubsystem(location), message);
    }

    /// Logs at error, tagged with the calling module's subsystem.
    void logError(std::string_view message, std::source_location location = std::source_location::current())
    {
        log(LogLevel::Error, logSubsystem(location), message);
    }
}
