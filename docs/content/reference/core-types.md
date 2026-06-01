+++
title = 'Core types'
weight = 1
math = false
+++

# Core types

Aliases, error type, and value types exported by `Saffron.Core`.

## Integer / float aliases
| Alias | Underlying |
|---|---|
| `u8` | `std::uint8_t` |
| `u16` | `std::uint16_t` |
| `u32` | `std::uint32_t` |
| `u64` | `std::uint64_t` |
| `i8` | `std::int8_t` |
| `i16` | `std::int16_t` |
| `i32` | `std::int32_t` |
| `i64` | `std::int64_t` |
| `f32` | `float` |
| `f64` | `double` |

## Error as value
| Symbol | Signature | Note |
|---|---|---|
| `Result<T>` | `using Result<T> = std::expected<T, std::string>` | success is the value, `{}` for `Result<void>` |
| `Err` | `auto Err(std::string message) -> std::unexpected<std::string>` | failure; no `Ok` wrapper |

## Reference
| Symbol | Signature | Note |
|---|---|---|
| `Ref<T>` | `using Ref<T> = std::shared_ptr<T>` | shared handle to a meta-layer resource (no base class) |

## Value types
| Type | Fields | Note |
|---|---|---|
| `TimeSpan` | `f32 seconds = 0.0f` | span of time |
| `Uuid` | `u64 value = 0` | stable 64-bit identity (entt ids are not stable across runs) |

## Functions
| Symbol | Signature | Effect |
|---|---|---|
| `toMilliseconds` | `constexpr auto toMilliseconds(TimeSpan span) -> f32` | `span.seconds * 1000` |
| `newUuid` | `auto newUuid() -> Uuid` | random nonzero u64 |
| `logInfo` | `void logInfo(std::string_view)` | `[saffron] …` |
| `logWarn` | `void logWarn(std::string_view)` | `[saffron] warn: …` |
| `logError` | `void logError(std::string_view)` | `[saffron] error: …` |

## Constants
| Symbol | Value |
|---|---|
| `EngineName` | `"Saffron Engine"` |
| `EngineVersion` | `"0.1.0-vulkan"` |

## Related
- [Error handling](../../explanations/core-and-conventions/error-handling/) — the `Result`/`Err` scheme
- [Type aliases and primitives](../../explanations/core-and-conventions/type-aliases-and-primitives/) — why the Go-style spellings
- [Ownership and RAII](../../explanations/core-and-conventions/ownership-and-raii/) — what `Ref<T>` points at
