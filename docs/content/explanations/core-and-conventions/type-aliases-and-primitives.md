+++
title = 'Type aliases'
weight = 3
+++

# Type aliases

A type alias gives a familiar type a shorter, clearer name. `Saffron.Core` defines a small
set of them — fixed-width number names and a few value types — that every other module
shares.

The names are short and spelled identically everywhere, so a declaration shows the width or
intent of its type at a glance. `Saffron.Core` sits at the bottom of the module DAG, which is
why these primitives live there.

## Number aliases

The integer and float types take short, Go-like names so a field declaration reads at the
same width as the type it holds:

```cpp
using u8  = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;
using f64 = double;
// … i8…i64, u16 …
```

The codebase prefers these to `int`, `unsigned`, `float`, and `size_t`. A vertex count is a
`u32`, a hash or identity is a `u64`, a shader push constant is an `f32`. The width is part
of the name, so a struct laid out for the GPU shows its byte layout directly.

## TimeSpan

A duration is a `TimeSpan`: a one-field struct holding seconds, with a free function to read
it in other units. This is the [Go-flavored](../go-flavored-design/) shape in miniature —
plain data and a free function that transforms it, rather than a method on the type. The
frame delta passed to a layer's `onUpdate` is a `TimeSpan`.

```cpp
struct TimeSpan
{
    f32 seconds = 0.0f;
};

constexpr auto toMilliseconds(TimeSpan span) -> f32 { return span.seconds * 1000.0f; }
```

## Uuid and newUuid

`Uuid` is a stable 64-bit identity. entt's own `entt::entity` values are not stable across
runs, since they are reused as entities are created and destroyed, so anything serialized and
reloaded carries a `Uuid` instead.

```cpp
auto newUuid() -> Uuid
{
    static std::mt19937_64 engine{ std::random_device{}() };
    static std::uniform_int_distribution<u64> distribution{ 1, std::numeric_limits<u64>::max() };
    return Uuid{ distribution(engine) };
}
```

`newUuid` draws from a static Mersenne Twister seeded once from `random_device`. The
distribution starts at 1, so a fresh `Uuid` is never zero; zero reads as "unset", the
default member initializer. Catalog assets and saved-scene entities are keyed by `Uuid`,
which is how a reloaded `project.json` reconnects a `MeshComponent` to the right mesh.

## In the code

| What | File | Symbols |
|---|---|---|
| Width aliases | `core.cppm` | `u8`…`u64`, `i8`…`i64`, `f32`, `f64` |
| Duration | `core.cppm` | `TimeSpan`, `toMilliseconds` |
| Stable identity | `core.cppm` | `Uuid`, `newUuid` |
| Engine name/version | `core.cppm` | `EngineName`, `EngineVersion` |

> [!NOTE]
> A default-constructed `Uuid` has `value == 0`, and `newUuid` never returns zero. Treat
> zero as the unset sentinel — don't compare a fresh `Uuid` against `Uuid{}` expecting a
> match.

## Related

- [Go-flavored design](../go-flavored-design/) — why a duration is a struct plus a free function
- [Ownership and RAII](../ownership-and-raii/) — `Ref<T>`, the other core alias
