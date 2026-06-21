+++
title = 'Core primitives'
weight = 3
+++

# Core primitives

`saffron-core` is the root of the crate DAG — it depends on no other Saffron crate, and every
other crate depends on it. It defines the handful of small value types the whole engine shares: a
stable identity newtype, a duration, the engine identity strings, the `Ref` ownership alias, and a
base64 helper for the control wire. Rust's own `u8`…`u64` / `f32` / `f64` are the numeric
vocabulary, so the only custom primitives here are the ones that carry engine meaning.

## Uuid — the stable identity

`Uuid` is a stable 64-bit identity, a newtype over `u64`:

```rust
pub struct Uuid(pub u64);

impl Uuid {
    pub fn new() -> Self { /* mint from [1024, u64::MAX] */ }
    pub fn value(self) -> u64 { self.0 }
}
```

A `hecs` ECS entity value is not stable across runs — entities are reused as they are created and
destroyed — so anything serialized and reloaded carries a `Uuid` instead. `Uuid::new` mints from a
per-thread SplitMix64 generator seeded once from a high-resolution clock; ids below `1024` are
reserved for built-in / synthetic assets (e.g. the default material), so a minted id never
collides with a reserved one. Catalog assets and saved-scene entities are keyed by `Uuid`, which is
how a reloaded project reconnects a mesh component to the right mesh.

> [!NOTE]
> A `Uuid`'s wire form is a **decimal string**, not a number — ids span the full `u64` range past
> JavaScript's `2^53` safe integer. The newtype carries no `serde` derive of its own; the encoding
> lives once in the [JSON gateway](../json-gateway/) (`WireUuid`) and the protocol crate.

## TimeSpan — a duration

A duration is a `TimeSpan`: a one-field struct over seconds, with `const` constructors and a unit
read. The frame delta passed to a layer's `on_update` is a `TimeSpan`.

```rust
pub struct TimeSpan {
    pub seconds: f32,
}

impl TimeSpan {
    pub const fn from_seconds(seconds: f32) -> Self { Self { seconds } }
    pub const fn to_milliseconds(self) -> f32 { self.seconds * 1000.0 }
}
```

## Ref — the ownership alias

`Ref<T>` is `Arc<T>`, the shared-read default of the [ownership policy](../ownership-and-raii/). It
lives in the core crate so every downstream crate names the shared-read shape the same way.

## base64_encode — small blobs on the wire

`base64_encode` renders a byte buffer as standard base64 (RFC 4648), used to carry small binary
blobs — thumbnail PNGs, say — over the JSON control plane.

## Engine identity

`ENGINE_NAME` (`"Saffron Anima"`) and `ENGINE_VERSION` (`"0.1.0-vulkan"`) are the two identity
constants the host reports.

## In the code

| What | File | Symbols |
|---|---|---|
| Stable identity | `engine/crates/core/src/uuid.rs` | `Uuid`, `Uuid::new`, `Uuid::value` |
| Duration | `engine/crates/core/src/time.rs` | `TimeSpan`, `from_seconds`, `to_milliseconds` |
| Ownership alias | `engine/crates/core/src/lib.rs` | `Ref` |
| Base64 helper | `engine/crates/core/src/base64.rs` | `base64_encode` |
| Engine identity | `engine/crates/core/src/lib.rs` | `ENGINE_NAME`, `ENGINE_VERSION` |

## Related

- [Rust house style](../go-flavored-design/) — why a duration is a struct plus methods
- [Ownership](../ownership-and-raii/) — `Ref<T>`, the shared-read alias
- [JSON gateway](../json-gateway/) — where `Uuid`'s decimal-string wire form is defined
