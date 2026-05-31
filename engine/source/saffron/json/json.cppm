module;

// nlohmann/json is a header-heavy C++ library, so this module uses classic includes
// (no `import std`), like the rendering/scene modules.
#include <nlohmann/json.hpp>

#include <charconv>
#include <expected>
#include <format>
#include <string>
#include <string_view>

export module Saffron.Json;

import Saffron.Core;

// A thin gateway over nlohmann/json. The engine builds json with JSON_NOEXCEPTION, so
// the library's own error path is `std::abort()` — a parse error, a `.dump()` on invalid
// UTF-8, or a typed read (`get<T>()`/`value<>`/`at()`) on the wrong type all crash the
// process. These wrappers convert every such failure into the engine's error-as-value
// style (`std::expected` / a checked default) so json input never aborts.
export namespace se
{
    using Json = nlohmann::json;

    /// Parse text into a Json value, or an error (never aborts).
    std::expected<Json, std::string> parseJson(std::string_view text);

    /// Serialize to a string; invalid UTF-8 is replaced rather than aborting. indent < 0
    /// is compact, >= 0 pretty-prints with that many spaces.
    std::string dumpJson(const Json& value, int indent = -1);

    /// Typed object-field reads. Each checks the value's type before extracting, so a
    /// missing key or a wrong type yields an error instead of aborting. jsonU64 also
    /// accepts a numeric string (the se CLI passes bare numbers as strings).
    std::expected<u64, std::string> jsonU64(const Json& object, std::string_view key);
    std::expected<std::string, std::string> jsonString(const Json& object, std::string_view key);
    std::expected<f64, std::string> jsonF64(const Json& object, std::string_view key);
    std::expected<bool, std::string> jsonBool(const Json& object, std::string_view key);

    /// The same reads, returning a fallback instead of an error when the key is absent or
    /// mistyped (the "value-or-default" pattern for optional fields).
    u64 jsonU64Or(const Json& object, std::string_view key, u64 fallback);
    std::string jsonStringOr(const Json& object, std::string_view key, std::string fallback);
    f32 jsonF32Or(const Json& object, std::string_view key, f32 fallback);
    bool jsonBoolOr(const Json& object, std::string_view key, bool fallback);
}

namespace se
{
    std::expected<Json, std::string> parseJson(std::string_view text)
    {
        Json value = Json::parse(text, nullptr, false);  // allow_exceptions = false
        if (value.is_discarded())
        {
            return std::unexpected(std::string{ "invalid JSON" });
        }
        return value;
    }

    std::string dumpJson(const Json& value, int indent)
    {
        return value.dump(indent, ' ', false, Json::error_handler_t::replace);
    }

    // Locate object[key]; null iterator semantics via end().
    namespace
    {
        Json::const_iterator findField(const Json& object, std::string_view key)
        {
            if (!object.is_object())
            {
                return object.end();
            }
            return object.find(std::string{ key });
        }
    }

    std::expected<u64, std::string> jsonU64(const Json& object, std::string_view key)
    {
        Json::const_iterator it = findField(object, key);
        if (it == object.end())
        {
            return std::unexpected(std::format("missing key '{}'", key));
        }
        if (it->is_number_unsigned())
        {
            return it->get<u64>();
        }
        if (it->is_number_integer())
        {
            const i64 signedValue = it->get<i64>();
            if (signedValue >= 0)
            {
                return static_cast<u64>(signedValue);
            }
        }
        if (it->is_string())
        {
            const std::string text = it->get<std::string>();
            u64 parsed = 0;
            const std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec == std::errc{} && result.ptr == text.data() + text.size())
            {
                return parsed;
            }
        }
        return std::unexpected(std::format("key '{}' is not an unsigned integer", key));
    }

    std::expected<std::string, std::string> jsonString(const Json& object, std::string_view key)
    {
        Json::const_iterator it = findField(object, key);
        if (it == object.end())
        {
            return std::unexpected(std::format("missing key '{}'", key));
        }
        if (it->is_string())
        {
            return it->get<std::string>();
        }
        return std::unexpected(std::format("key '{}' is not a string", key));
    }

    std::expected<f64, std::string> jsonF64(const Json& object, std::string_view key)
    {
        Json::const_iterator it = findField(object, key);
        if (it == object.end())
        {
            return std::unexpected(std::format("missing key '{}'", key));
        }
        if (it->is_number())
        {
            return it->get<f64>();
        }
        return std::unexpected(std::format("key '{}' is not a number", key));
    }

    std::expected<bool, std::string> jsonBool(const Json& object, std::string_view key)
    {
        Json::const_iterator it = findField(object, key);
        if (it == object.end())
        {
            return std::unexpected(std::format("missing key '{}'", key));
        }
        if (it->is_boolean())
        {
            return it->get<bool>();
        }
        return std::unexpected(std::format("key '{}' is not a boolean", key));
    }

    u64 jsonU64Or(const Json& object, std::string_view key, u64 fallback)
    {
        std::expected<u64, std::string> value = jsonU64(object, key);
        if (value)
        {
            return *value;
        }
        return fallback;
    }

    std::string jsonStringOr(const Json& object, std::string_view key, std::string fallback)
    {
        std::expected<std::string, std::string> value = jsonString(object, key);
        if (value)
        {
            return *value;
        }
        return fallback;
    }

    f32 jsonF32Or(const Json& object, std::string_view key, f32 fallback)
    {
        std::expected<f64, std::string> value = jsonF64(object, key);
        if (value)
        {
            return static_cast<f32>(*value);
        }
        return fallback;
    }

    bool jsonBoolOr(const Json& object, std::string_view key, bool fallback)
    {
        std::expected<bool, std::string> value = jsonBool(object, key);
        if (value)
        {
            return *value;
        }
        return fallback;
    }
}
