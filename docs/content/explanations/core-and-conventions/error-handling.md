+++
title = 'Error handling'
weight = 2
+++

# Error handling

Error handling in the engine is value-based: every operation that can fail returns a typed
`Result`, propagated with the `?` operator, never a panic on the expected-failure path. The
failure stays visible in ordinary control flow instead of unwinding invisibly.

Each library crate owns its own error type. `saffron-core` defines the root:

```rust
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{0}")]
    Message(String),
}

pub type Result<T> = core::result::Result<T, Error>;
```

The error is a [`thiserror`](https://docs.rs/thiserror) `enum`: each variant carries its own
fields and renders its own `Display` message through the `#[error("…")]` attribute. Success is
the value; failure is a variant. There is no string-typed catch-all dressed up as an error —
where a failure genuinely has structure, the variant carries it.

## A type per crate, composed with `#[from]`

`saffron-core::Error` has almost no fallible functions of its own; its value is that downstream
crates compose against it. Every library crate exports its own `Error` enum and a `Result<T>`
alias bound to it. A crate that calls into another lifts the callee's error into its own with a
`#[from]` variant, so `?` converts as it propagates:

```rust
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error(transparent)]
    Renderer(#[from] saffron_rendering::Error),
    // … this crate's own variants …
}
```

`saffron-json` shows the structured-variant style: a parse failure, a missing key, and a
wrong-type read are three distinct variants, each with the fields a caller needs to react.

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

## How it reads at the call site

A `Result` is handled at the call site: propagate it with `?`, or match when the function reacts
differently to different failures. Without unwinding, cleanup is ordinary `Drop` — a value
dropped on an early return frees itself, so the failure path needs no manual teardown.

```rust
let value = json::parse_json(text)?;       // propagate
let id = json::json_u64(&value, "id")?;    // typed, structured error on miss/mistype
```

The app's `run` shows this at scale: the window, renderer, and frame host are each built and the
error propagated in turn; a failure short-circuits and every already-built resource drops on the
way out.

## The third-party boundary

The no-panic-on-failure rule extends to the libraries the engine wraps. They are driven through
their `Result`-returning surfaces and lifted into the wrapping crate's error at the boundary:
`serde_json` parse/serialize errors become `json::Error::Parse`; `ash` Vulkan calls return
`VkResult`, checked and converted in `saffron-rendering`; `vk-mem` allocation failures convert
the same way. The `?` operator carries each across the seam.

> [!NOTE]
> Panics are reserved for invariants the type system can't express — never for a foreseeable
> failure like a bad file or a failed Vulkan call. Those are `Result`. The workspace keeps
> `panic = "unwind"` so the FFI/shm seams unwind cleanly and `#[should_panic]` tests work.

## In the code

| What | File | Symbols |
|---|---|---|
| The root error + alias | `engine/crates/core/src/error.rs` | `Error`, `Result` |
| Structured-variant errors | `engine/crates/json/src/lib.rs` | `Error::Parse`, `Error::MissingKey`, `Error::WrongType` |
| Composing with `#[from]` | `engine/crates/app/src/lib.rs` | `Error::Renderer(#[from] …)` |
| A real chain of checks | `engine/crates/app/src/lib.rs` | `run` — window → renderer → frame host |

## Related

- [Rust house style](../go-flavored-design/) — the style this is part of
- [JSON gateway](../json-gateway/) — typed reads return these errors
