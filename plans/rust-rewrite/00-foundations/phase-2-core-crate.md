# Phase 2 — `saffron-core`: the leaf primitives and conventions anchor

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-1-workspace-scaffold

## Goal

Port `Saffron.Core` into the `saffron-core` crate: the identity/time primitives, the `Ref`/`Result`
aliases, the logging functions, and base64. This is the DAG root every other crate depends on, so it
is where the house-style decisions from `conventions.md` first become real code — `Result` as a typed
alias, `Ref = Arc`, `Uuid(u64)` as a newtype, `log*` as subsystem-tagged functions. Getting these
exact makes the ~20 downstream crates mechanical.

The crate is tiny (~170 lines of C++) but high-leverage. The trap to avoid is transliterating C++
habits: `Result<T>` is *not* `Result<T, String>` here, and `Ref` is `Arc` (not a blanket
`Arc<Mutex>`).

## Why this shape (NO LEGACY)

- **`Result<T>` is bound to a typed `saffron_core::Error`, not to `String`.** The C++
  `Result<T> = std::expected<T, std::string>` was the only error model available without exceptions;
  Rust has typed errors, so the alias is `pub type Result<T> = core::result::Result<T, Error>` with a
  `thiserror` `Error` enum. `saffron-core`'s own error set is small (it has almost no fallible
  functions of its own); the value is that downstream crates compose against a typed root via
  `#[from]`. Carrying `Result<T, String>` would import a C++ workaround for no reason
  (`conventions.md` §2).
- **`Ref<T> = Arc<T>`, full stop, at this layer.** The C++ `Ref` alias (`core.cppm:45`) is
  `shared_ptr`; here it is a `pub type Ref<T> = Arc<T>` alias kept *only as a readability convenience*
  for the common read-shared case. Shared-*mutable* sites do **not** use `Ref` — they spell
  `Arc<Mutex<T>>` explicitly at their declaration (`conventions.md` §3). So `Ref` carries the
  read-shared default and the explicit `Arc<Mutex>` carries the exception; there is one rule, visible
  at each site.
- **`Uuid` is a newtype with the `<1024` reservation preserved and the decimal-string wire note
  attached.** The mint range `[1024, u64::MAX]` (the built-in/synthetic reservation, `core.cppm:84`)
  is reproduced. The decimal-string JSON encoding is *not* implemented here (that is the protocol
  crate's `serde_with` attribute, PP-7) — `saffron-core` defines the newtype and a `///` doc note
  pointing at the frozen wire contract, so there is exactly one place the encoding is decided.
- **Logging stays free functions, but the subsystem tag is derived idiomatically.** The C++
  `logSubsystem` parses `std::source_location::file_name()` for the module dir (`core.cppm:9`). The
  Rust port uses the standard `log` facade (or a thin equivalent) with the subsystem taken from
  `module_path!()` / the crate name, not by string-scraping a file path — same `[saffron:subsystem]`
  output format, idiomatic mechanism. The `[saffron:<subsystem>] message` / `warn:` / `error:` line
  format is preserved (it is grep-relied-upon and the validation-clean-log gate reads it).
- **`base64Encode` ports 1:1** as a function over `&[u8] -> String` (RFC 4648 standard table, the same
  used for thumbnail-PNG-over-JSON, `core.cppm:90`); a maintained `base64` crate is acceptable since
  the table is the standard one — pin in PP-2 — or hand-rolled to match exactly. Decode is added only
  if a reader needs it (the C++ has encode only).
- **No `runSignalSelfTest`-style runtime self-test** anywhere; `saffron-core`'s own behavior (base64
  round-trip, uuid range) is covered by `#[cfg(test)]` units (`conventions.md` §8).

## Grounding (real files/symbols)

- `engine-old/source/saffron/core/core.cppm`
  - Aliases: `u8..f64`, `Ref<T>` (`:45`), `Result<T>`/`Err` (`:51`/`:53`), `EngineName`/`EngineVersion`
    (`:58`/`:59`).
  - Time/identity: `TimeSpan` + `toMilliseconds` (`:62`/`:67`), `Uuid` (`:74`), `newUuid` with the
    `[1024, max]` reservation (`:79`/`:84`).
  - base64: `base64Encode` (`:90`).
  - Logging: `LogLevel` (`:124`), `log` (`:135`), `logInfo`/`logWarn`/`logError` (`:152`/`:158`/`:164`),
    `logSubsystem` path→subsystem mapping (`:9`).
- `conventions.md` §2 (typed `Result`), §3 (`Ref`→`Arc` policy), §7 (`Uuid` + decimal-string note).
- Downstream consumers proving the surface is load-bearing: `logInfo/Warn/Error` ~252 call sites;
  `Uuid`/`newUuid` ~164 sites; `base64Encode` in `control_commands_asset.cpp` thumbnail replies.

## Acceptance gate

- `cargo build -p saffron-core` and the full workspace build are green.
- `cargo test -p saffron-core` passes named unit tests:
  - `base64Encode` matches the C++ output for the empty buffer, 1/2/3-byte tails (padding `=`/`==`),
    and a known multi-byte vector (golden vector lifted from a thumbnail-reply round-trip).
  - `Uuid::new()` always returns `>= 1024` (the reservation holds) and two mints differ.
  - `TimeSpan`/`to_milliseconds` arithmetic matches (`seconds * 1000`).
- `cargo clippy -p saffron-core` and `cargo fmt --check` clean; the crate root carries
  `#![deny(unsafe_code)]`.
- A log line from any function renders exactly `[saffron:core] <message>` (and `warn:`/`error:`
  prefixes for the non-info levels) — asserted by a capture test or reviewed against `core.cppm:135`.
