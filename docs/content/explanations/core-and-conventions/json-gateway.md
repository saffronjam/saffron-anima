+++
title = 'JSON gateway'
weight = 7
+++

# JSON gateway

The JSON gateway is a thin wrapper around nlohmann/json that converts every fallible JSON
operation into the engine's [error-as-value style](../error-handling/). Scene and project
files pass through it; engine code never calls the library directly.

The wrapper exists because of how nlohmann/json is configured here. The library compiles
with `JSON_NOEXCEPTION`, which means its own error path is `std::abort()` rather than a
thrown exception. A parse error, a `.dump()` on invalid UTF-8, or a typed read such as
`get<T>()` or `at()` on the wrong type kills the process outright. The gateway turns each
operation that would abort into a `Result` or a checked default, so untrusted JSON cannot
take down the editor.

## Parse and dump

Parsing uses nlohmann's `allow_exceptions = false` overload, which returns a discarded
value instead of aborting. The gateway maps that discarded value to an `Err`:

```cpp
auto parseJson(std::string_view text) -> Result<Json>
{
    Json value = Json::parse(text, nullptr, false);  // allow_exceptions = false
    if (value.is_discarded()) { return Err(std::string{ "invalid JSON" }); }
    return value;
}
```

Serializing is the mirror image. `.dump()` aborts on invalid UTF-8, so `dumpJson` passes
`error_handler_t::replace`, substituting the replacement character instead of dying. A
negative indent produces compact output; zero or more pretty-prints with that many spaces.

## Typed reads

A typed read asks a value for a type it may not hold, such as a `u64`. The field readers
locate the key, verify the stored type, and only then extract. A missing key and a wrong
type each become a descriptive `Err`, never an abort.

```cpp
auto jsonString(const Json& object, std::string_view key) -> Result<std::string>
{
    Json::const_iterator it = findField(object, key);
    if (it == object.end()) { return Err(std::format("missing key '{}'", key)); }
    if (it->is_string())    { return it->get<std::string>(); }
    return Err(std::format("key '{}' is not a string", key));
}
```

`jsonU64` accepts the widest range of inputs: an unsigned number, a non-negative signed
number, and a numeric string. The `se` CLI passes bare numbers across the socket as
strings, and the gateway absorbs that conversion.

## Value-or-default variant

Optional fields do not want a `Result` at every call site. Each reader therefore has an `Or`
twin that swallows the error and returns a fallback: `jsonU64Or`, `jsonStringOr`,
`jsonF32Or`, `jsonBoolOr`. Scene and project loading rely on these. A field absent from an
older save reads as its default rather than failing the whole load, which is how a
[project file](../../geometry-and-assets/project-serialization/) stays forward-compatible as
components gain fields.

## In the code

| What | File | Symbols |
|---|---|---|
| Gateway rationale | `json.cppm` | module doc (`JSON_NOEXCEPTION` → abort) |
| Parse / serialize | `json.cppm` | `parseJson`, `dumpJson` |
| Checked typed reads | `json.cppm` | `jsonU64`, `jsonString`, `jsonF64`, `jsonBool` |
| Value-or-default reads | `json.cppm` | `jsonU64Or`, `jsonStringOr`, `jsonF32Or`, `jsonBoolOr` |

> [!WARNING]
> Reach for `Json::get<T>()`, `at()`, or `.dump()` directly and a malformed value aborts
> the process — `JSON_NOEXCEPTION` has no throw to catch. Always go through the gateway's
> checked readers; they are the reason bad input fails gracefully instead of crashing.

## Related

- [Error handling](../error-handling/) — the `Result`/`Err` style the gateway converts into
- [Scene serialization](../../scene-and-ecs/scene-serialization/) — the registry-driven save/load built on these readers
- [Project serialization](../../geometry-and-assets/project-serialization/) — the unified `project.json`
