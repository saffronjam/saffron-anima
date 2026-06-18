# Phase 4 — `saffron-json`: the serde gateway and the decimal-string-u64 contract

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-2-core-crate

## Goal

Port `Saffron.Json` into `saffron-json`: the thin JSON gateway the engine uses for the control wire,
scene/project files, and small binary-over-JSON blobs. In C++ this module exists *because* nlohmann is
built with `JSON_NOEXCEPTION` (its error path is `std::abort()`), so every parse/dump/typed-read is
wrapped to return `Result` instead of crashing. In Rust that whole firewall reason disappears — serde
returns `Result` natively — so the crate shrinks to: re-export the serde value type, expose the same
fallible-read helper surface the engine calls, and **own the decimal-string-u64 wire encoding** that
the rest of the engine and the editor depend on byte-for-byte.

This is the first place the frozen wire contract is touched. The load-bearing detail: u64 ids cross
the wire as *decimal strings* (JS 2^53 limit) and are read leniently (string **or** number). Getting
this wrong fails silently — a JSON number passes every type check and corrupts the id on a JS client.

## Why this shape (NO LEGACY)

- **The `JSON_NOEXCEPTION` abort firewall is deleted, not ported.** The entire raison d'être of
  `Saffron.Json` (`json.cppm` header comment: "the library's own error path is `std::abort()` … these
  wrappers convert every such failure into the engine's error-as-value style") evaporates because
  `serde_json` returns `Result`. So `parse_json`/`dump_json` become thin wrappers over
  `serde_json::from_str`/`to_string(_pretty)` returning the crate's typed `Result` — no abort-guarding,
  no discarded-value sentinel.
- **The typed-read helpers are kept as a deliberate API**, not replaced by ad-hoc `serde` deriving at
  every call site — because the *control command handlers* read params field-by-field with these exact
  lenient semantics, and the `*Or` (value-or-default) readers encode optional-field behavior the wire
  contract relies on. `json_u64`/`json_string`/`json_f64`/`json_bool` (strict, return a typed error on
  missing/mistyped) and `json_u64_or`/`json_string_or`/`json_f32_or`/`json_bool_or` (fallback on
  missing/mistyped) port 1:1. They operate over `serde_json::Value` exactly as the C++ ones operate
  over `nlohmann::json`.
- **`json_u64` accepts a number, a non-negative integer, *or* a decimal string** — the exact lenient
  union the C++ `jsonU64` implements (`json.cppm:92`: `is_number_unsigned` → `is_number_integer >= 0`
  → `is_string` parsed by `from_chars` requiring the *whole* string to consume). The Rust port
  reproduces all three branches, including the "entire string must parse" strictness (a trailing-garbage
  string is rejected). This is the read side of the frozen contract.
- **`uuid_to_json` emits a decimal *string***, never a number (`json.cppm:72` — `std::to_string`, with
  the explicit nlohmann note that brace-init would make a one-element array; the Rust equivalent is
  just `Value::String(value.to_string())`). The DTO-level `serde_with::PickFirst<(DisplayFromStr, _)>`
  attribute (the derive-driven version) lives in `saffron-protocol` (PP-7); `saffron-json` provides the
  imperative helper the hand-written control-command readers use. There is one wire encoding, defined
  by these two functions, and the protocol crate's derive must match them byte-for-byte (a contract
  test in PP-7/PP-13 cross-checks).
- **`dump_json` replaces invalid UTF-8 rather than failing** — the C++ uses nlohmann's
  `error_handler_t::replace` (`json.cppm:69`); serde_json's strings are already valid UTF-8 (Rust
  `String` is UTF-8 by construction), so the replace-vs-abort concern is moot, but the pretty/compact
  `indent < 0 ? compact : pretty(indent)` selection is preserved.
- **No self-test function.** The lenient-read and decimal-string behaviors are covered by
  `#[cfg(test)]` units (`conventions.md` §8).

## Grounding (real files/symbols)

- `engine-old/source/saffron/json/json.cppm`
  - The firewall rationale: the module header comment (`:17`–`:21`) — the reason the module exists,
    which Rust deletes.
  - `parse_json` ← `parseJson` (`:57`, `allow_exceptions=false` + discarded-value check),
    `dump_json` ← `dumpJson` (`:67`, indent + replace).
  - The wire contract: `uuid_to_json` ← `uuidToJson` (`:72`, decimal string); `json_u64` ← `jsonU64`
    (`:92`, the three-branch lenient union with whole-string `from_chars`).
  - The strict readers: `jsonString` (`:124`), `jsonF64` (`:138`), `jsonBool` (`:152`); the `findField`
    helper (`:82`).
  - The `*Or` fallback readers: `jsonU64Or` (`:166`), `jsonStringOr` (`:176`), `jsonF32Or` (`:186`,
    note the f64-read-then-narrow-to-f32), `jsonBoolOr` (`:196`).
- Consumers proving the lenient reads are load-bearing: `readWireUuid`/`uuidToJson` in
  `engine-old/source/saffron/control/control_dto_serde.generated.cpp` (the wire round-trip);
  the `control_commands_*.cpp` handlers reading params field-by-field.
- `conventions.md` §7 (the `Uuid` decimal-string contract, owned jointly with PP-7), §2 (typed `Result`).

## Acceptance gate

- `cargo build -p saffron-json` and the full workspace build are green;
  `saffron-json` depends on `saffron-core` and `serde`/`serde_json` (pinned in the workspace table).
- `cargo test -p saffron-json` passes named unit tests:
  - **decimal-string emit** — `uuid_to_json(u64::MAX)` is the JSON *string* `"18446744073709551615"`,
    not a number (the silent-failure guard: assert the serialized bytes contain quotes).
  - **lenient u64 read** — `json_u64` accepts `{"k": 42}` (number), `{"k": "42"}` (decimal string), and
    a full-u64-range string; rejects `{"k": "42x"}` (trailing garbage), a negative number, and a
    missing key (typed error). Mirrors the three-branch `jsonU64`.
  - **strict readers** — `json_string`/`json_f64`/`json_bool` return a typed error on missing key and
    on wrong type.
  - **`*Or` fallback** — each `*_or` returns the fallback on missing/mistyped and the value otherwise;
    `json_f32_or` narrows an f64 wire value to f32.
- `cargo clippy -p saffron-json` and `cargo fmt --check` clean; crate root `#![deny(unsafe_code)]`.
- The emitted bytes for a `Uuid` round-trip (`uuid_to_json` then `json_u64`) match what the existing
  `control_dto_serde.generated.cpp` `readWireUuid`/`uuidToJson` produce — cross-checked against a
  fixture so the protocol crate (PP-7) can rely on byte-equality.
