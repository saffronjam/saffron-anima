# Phase 2 — `build_params` + `coerce`: the positional/flag mapping and value coercion

**Status:** COMPLETED

**Depends on:** 11-sa-cli:phase-1-crate-and-socket-client

## Goal

Port `buildParams` and `coerce` (`main.cpp:40`,`:87`) verbatim as two pure, unit-tested functions that
turn the free-form `[args...]` captured by clap into the `params` object of the request envelope: bare
tokens into `params.args[]`, `--key value`/`--key=value` into `params[key]`, a bare `--key` into
`params[key] = true`, and each value coerced by the fixed type-precedence ladder. After this phase
`sa set-camera 1 2 3`, `sa set-camera --yaw 90`, `sa add-component --json '{"a":1}'`, and
`sa set-aa --enabled` all build the exact `params` the C++ CLI built.

## Why this shape (NO LEGACY)

- **This is the one wire behavior the CLI itself originates, so it ports byte-identically.** Everything
  else the CLI does is transport and presentation; `build_params` is the client mirror of the server's
  lenient/positional read (area-09 README §6). The engine's typed handlers deserialize *exactly* what
  this emits — a positional `1` must reach the first DTO field, a `--yaw 90` must reach the `yaw` field,
  `--enabled` (no value) must become `true`. If the coercion ladder or the positional/flag split drifts,
  a command silently sends the wrong params and the handler either errors or, worse, mis-parses. So this
  is a faithful 1:1 port with a `#[test]` per coercion branch, not a re-derivation.
- **It is pure-function and engine-free, so it tests without a socket.** `coerce(&str) -> Value` and
  `build_params(&[String]) -> Value` take strings and return `serde_json::Value`; no I/O, no engine. This
  is why it is its own phase — the whole coercion contract is provable in unit tests before any running
  engine exists, and it keeps the transport phase (phase-1) free of parsing logic.
- **`serde_json::Value` replaces `nlohmann::json` with the same coercion semantics.** The C++ `coerce`
  uses `strtoull`/`strtoll`/`strtod` whole-string parses (`end == '\0'` checks) and `json::parse(..., false)`
  (discard-on-error) for the explicit-JSON branch. The Rust port reproduces each:
  - `"true"`/`"false"`/`"null"` → `Value::Bool`/`Value::Null` literal match.
  - starts with `{`/`[`/`"` → `serde_json::from_str`, and **on parse error fall through** to the numeric
    ladder (the C++ `is_discarded()` check, `main.cpp:56` — a malformed `{...}` does not become a string
    immediately; it falls through, though in practice it then fails the numeric parses and ends as the
    bare string).
  - unsigned-first (only if the token does not start with `-`, `main.cpp:63`): `u64::from_str` on the
    whole string → `Value::from(u64)`. This ordering matters — a large positive integer must serialize as
    an unsigned number, and an id-like decimal must not be lossily widened; the C++ tries `strtoull`
    before `strtoll` for exactly this.
  - signed: `i64::from_str` → `Value::from(i64)`.
  - float: `f64::from_str` → `Value::from(f64)`.
  - else: `Value::String(token)`.
- **The flag-vs-positional split is the exact `buildParams` walk.** A token starting `--` is a flag:
  split on the first `=` for `--key=value`; else if the *next* token does not start `--`, consume it as
  the value; else it is a bare `--key = true`. Non-`--` tokens are positionals appended to the `args`
  array, which is added to `params` only if non-empty (`main.cpp:120`). This index walk ports directly;
  the only Rust nicety is iterating with an explicit index (a `while i < len` loop) to mirror the C++
  two-step advance.

## Grounding (real files / symbols)

- `tools/sa/source/main.cpp`: `coerce` (40) — the literal/JSON/unsigned/signed/float/string precedence,
  including the `front() != '-'` guard before the unsigned parse (63) and the `is_discarded()` fall-through
  (56–60); `buildParams` (87) — the `rfind("--", 0) == 0` flag test, the `=`-split (97), the next-token
  consume vs bare-flag-true (103–112), the positional `args` array (114–123).
- `tools/sa/AGENTS.md`: the "Argument parsing" section — the precise precedence list and the
  bare-`--key` → `true` rule (the contract this phase preserves).
- `09-control-plane/catalog.md`: the note that field declaration order == positional CLI arg order — the
  reason `params.args[]` ordering is load-bearing (the engine maps it back to DTO fields by index).

## Acceptance gate

- `cargo build --workspace` succeeds; clippy + fmt clean; `#![deny(unsafe_code)]` holds.
- `#[test]`s over `coerce` cover every branch and the precedence boundaries: `"true"`/`"false"`/`"null"`
  → the literals; `"42"` → unsigned number; `"-42"` → signed number; `"3.14"` → float;
  `"18446744073709551615"` (u64::MAX) → unsigned number (not float, not string — proves unsigned-first);
  `"-5"` does **not** take the unsigned path (the `-` guard); `"{\"a\":1}"` / `"[1,2]"` / `"\"hi\""` →
  parsed JSON; a malformed `"{nope"` falls through to the bare string; `"foo"` → string.
- `#[test]`s over `build_params` reproduce: `["1","2","3"]` → `{"args":[1,2,3]}`; `["--yaw","90"]` →
  `{"yaw":90}`; `["--yaw=90"]` → `{"yaw":90}`; `["--enabled"]` → `{"enabled":true}`;
  `["cube","--yaw","90","extra"]` → `{"args":["cube","extra"],"yaw":90}` (positionals collected, flag
  pulled out); `[]` → `{}` (empty object, no `args` key); `["--a","--b","x"]` → `{"a":true,"b":"x"}` (a
  bare flag followed by another flag, vs a flag followed by a value).
- A `#[test]` asserts the built request for `sa set-camera 1 2 3 --fov 60` equals
  `{"cmd":"set-camera","params":{"args":[1,2,3],"fov":60},"id":1}` (phase-1 envelope + phase-2 params
  composed).
