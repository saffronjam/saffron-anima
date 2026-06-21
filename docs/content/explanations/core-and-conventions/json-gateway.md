+++
title = 'JSON gateway'
weight = 7
+++

# JSON gateway

The JSON gateway is a thin layer over [`serde_json`](https://docs.rs/serde_json) that gives the
engine one parse/dump entry point, a set of lenient typed readers the control-command handlers
depend on, and the decimal-string-`u64` wire encoding the engine and editor share byte-for-byte.
Scene and project files and every control message pass through it; engine code reaches `serde_json`
through this crate.

Every fallible operation returns the crate's typed [`Result`](../error-handling/) over a structured
error — a parse failure, a missing key, and a wrong-type read are three distinct variants, so a
caller can react to each:

```rust
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("invalid JSON: {0}")]
    Parse(String),
    #[error("missing key '{0}'")]
    MissingKey(String),
    #[error("key '{key}' is not {expected}")]
    WrongType { key: String, expected: &'static str },
}
```

## Parse and dump

`parse_json` wraps `serde_json::from_str` and maps a parse error to `Error::Parse`. `dump_json`
serializes a `Value`: a negative indent produces compact output, zero or more pretty-prints with
that many spaces per level.

```rust
pub fn parse_json(text: &str) -> Result<Value>;
pub fn dump_json(value: &Value, indent: i32) -> String;
```

`dump_json_sorted` is the asset-format variant: it re-emits every object's keys in
lexicographically sorted order, recursively. `serde_json` is built workspace-wide with
`preserve_order` (the control wire needs insertion order = field order), so the byte-frozen asset
formats — `.smat`, the `.smodel` META chunk — call `dump_json_sorted` instead to keep their stable
source hash.

## Typed reads

A typed read asks a value for a type it may not hold. Each reader locates the key, checks the stored
type, and only then extracts; a missing key is `Error::MissingKey`, a wrong type is
`Error::WrongType`, never a panic.

```rust
pub fn json_u64(object: &Value, key: &str) -> Result<u64>;
pub fn json_string(object: &Value, key: &str) -> Result<String>;
pub fn json_f64(object: &Value, key: &str) -> Result<f64>;
pub fn json_bool(object: &Value, key: &str) -> Result<bool>;
```

`json_u64` accepts the widest range of inputs: an unsigned number, a non-negative integer, or a
decimal string whose *entire* content parses. Ids cross the control wire as strings (below), so
accepting both number and string is what lets a value load either way; a trailing-garbage string
(`"42x"`) or a negative number is rejected.

## Ids as strings

A `u64` id spans the full 64-bit range, past the `2^53` a JavaScript number holds exactly. Emitted
as a JSON number, an id larger than that is silently rounded the moment a JS client runs the
response through `JSON.parse`. `uuid_to_json` serializes every id as a decimal JSON *string*, which
survives `JSON.parse` losslessly. `WireUuid` is the `serde_with` adapter that drives the same
encoding from a derive — `#[serde_as(as = "WireUuid")]` on a `Uuid` field emits the decimal string
and accepts a string *or* a number on read. The encoding is defined once here, and the protocol
crate reuses `WireUuid`, so there is exactly one wire form for every id on the control wire and in a
saved scene or project file.

## Value-or-default variant

Optional fields do not want a `Result` at every call site. Each strict reader has an `_or` twin that
swallows the error and returns a fallback: `json_u64_or`, `json_string_or`, `json_f32_or`,
`json_bool_or` (the last two read the `f64` wire value and narrow / coerce). Scene and project
loading rely on these: a field absent from a save reads as its default rather than failing the whole
load, which is how a [project file](../../geometry-and-assets/project-serialization/) stays
forward-compatible as components gain fields.

## In the code

| What | File | Symbols |
|---|---|---|
| Typed gateway error | `engine/crates/json/src/lib.rs` | `Error`, `Result` |
| Parse / serialize | `engine/crates/json/src/lib.rs` | `parse_json`, `dump_json`, `dump_json_sorted` |
| Id wire encoding | `engine/crates/json/src/lib.rs` | `uuid_to_json`, `WireUuid` |
| Checked typed reads | `engine/crates/json/src/lib.rs` | `json_u64`, `json_string`, `json_f64`, `json_bool` |
| Value-or-default reads | `engine/crates/json/src/lib.rs` | `json_u64_or`, `json_string_or`, `json_f32_or`, `json_bool_or` |

## Related

- [Error handling](../error-handling/) — the typed `Result` style the readers return
- [Core primitives](../type-aliases-and-primitives/) — `Uuid`, whose decimal-string wire form `WireUuid` defines
- [Scene serialization](../../scene-and-ecs/scene-serialization/) — the registry-driven save/load built on these readers
- [Project serialization](../../geometry-and-assets/project-serialization/) — the unified project file
