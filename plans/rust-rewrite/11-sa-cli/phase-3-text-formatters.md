# Phase 3 — text mode: the `help` table, the ~35 command-keyed formatters, and the UTF-8 fallback

**Status:** COMPLETED

**Depends on:** 11-sa-cli:phase-1-crate-and-socket-client, 11-sa-cli:phase-2-param-coercion

## Goal

Port `printResult` (`main.cpp:127`–489) — the `text` output mode. A closed `match cmd` over the command
name selects one of ~35 bespoke one-line/table formatters; everything unmatched falls through to pretty
JSON with non-ASCII **unescaped** (the `dump(2, ' ', false)` analogue). Plus the two structural pieces:
the `help` two-column table and the `profiler.capture-stop` inline-trace temp-file write. After this
phase `sa render-stats`, `sa list-entities`, `sa get-asset-model <id>`, `sa help`, etc. print the same
human-readable lines the C++ CLI printed, and `-o json` (phase-1) still bypasses all of it.

## Why this shape (NO LEGACY)

- **A `match cmd` over the name, reading a `&Value` with lenient field defaults — the exact C++ shape.**
  Each C++ arm reads `result.value("key", default)` (returns the default on a missing/wrong-typed key),
  so an arm never panics when the engine omits a field. The Rust port is a `match cmd.as_str()` whose
  arms read through a small helper set over `&serde_json::Value` (`val.get(k).and_then(Value::as_str)
  .unwrap_or("")`, `.as_f64().unwrap_or(0.0)`, `.as_i64()`, `.as_bool()`, `.as_array()`, etc.). These
  helpers are the `.value(key, default)` analogue and are written once, not inlined per arm. The
  formatters are pure presentation over `&Value` — no engine types, no I/O (except the one capture-stop
  file write) — so they are directly `#[test]`-coverable with hand-built `Value` fixtures.
- **The formatter set is closed and keyed on the command string, not on a result type.** The CLI does not
  type results (README §4), so it cannot dispatch on a DTO type — it dispatches on the command name, the
  same way the C++ `printResult(cmd, result, mode)` does. This keeps the CLI engine-free and means a
  result-shape change in the engine that adds a field is invisible here (the arm reads its known fields
  with defaults). **Adding a formatter is one `match` arm** — the C++ `AGENTS.md` rule preserved.
- **The fallback is UTF-8-unescaped pretty JSON, and that is deliberate.** The default text branch uses
  `dump(2, ' ', false)` so em-dashes and other non-ASCII render literally (`main.cpp:488`); `-o json`
  uses `dump(2)` (ASCII-escaped, jq-safe). `serde_json::to_string_pretty` does **not** escape non-ASCII
  by default, so it is the UTF-8-unescaped fallback directly; the `json` mode is the same call (serde
  emits valid UTF-8 JSON either way — there is no ASCII-only switch needed because the only reason the
  C++ had two was nlohmann's default ASCII-escaping, which serde does not do). The phase pins: both modes
  use `to_string_pretty`; the *only* difference is text mode runs the `match` first and falls through to
  `to_string_pretty`, while json mode goes straight to `to_string_pretty`.
- **`profiler.capture-stop` is the one formatter with a side effect, ported intact.** When the reply has
  no `path` but carries an inline `chromeTrace` string, the C++ writes it to
  `<temp_dir>/saffron-profile.json` and prints that path (`main.cpp:262`–276). The Rust port uses
  `std::env::temp_dir().join("saffron-profile.json")` + `std::fs::write`; the crate stays
  `#![deny(unsafe_code)]` (plain std fs). All other formatters are read-only.
- **The bone-tree indent walk (`get-asset-model`) ports with its guard.** That arm walks each bone's
  `parent` chain to compute an indent depth, with a 256-iteration cycle guard (`main.cpp:188`). It is the
  one non-trivial formatter; it ports as a small loop over the `bones` array with the same guard, so a
  malformed/cyclic parent index cannot hang the CLI.

## Grounding (real files / symbols)

- `tools/sa/source/main.cpp`: `printResult` (127) — the full `cmd ==` chain; the JSON-mode early return
  (129); the `help` table (134), `list-entities` (148), `list-components` (157), `list-assets` (165),
  `get-asset-model` with the bone-tree walk + clip list (174–202), `list-clips` (203), `render-stats`
  (212), `profiler.set-mode` (222), `pass-timings` (230), `profiler.capture-start` (243),
  `profiler.capture-stop` with the temp-file write (249–278), `frame-history` (279), perf-config (288),
  `drain-alarms` (298), `list-active-alarms` (313), play-state group (330), `physics-state` (337),
  `fit-collider` (343), `raycast`/`shapecast` (353), ragdoll group (370), `move-character` (378),
  `set-kinematic-bones` (385), `drain-contacts` (391), `viewport-native-info` (405), `set-active-view`
  (412), `get-selection` (417), `add-entity`/`copy-entity` (431), gizmo group (436), `gizmo-pointer`
  (441), `pick` (447), `pick-skeleton-joint` (460), camera group (472), thumbnail group (480), and the
  UTF-8-unescaped fallback (487–488).
- `tools/sa/AGENTS.md`: the output-mode table (text vs json) and "Adding a text formatter" (the `match`-arm
  rule); the note that text mode pretty-prints with UTF-8 unescaped.
- `09-control-plane/catalog.md`: the result-DTO field names each formatter reads (e.g. `RenderStatsDto`,
  `PlayStateResult`, `AssetModelResult.bones/clips/capabilities`) — the fields the arms key on.

## Acceptance gate

- `cargo build --workspace` succeeds; clippy + fmt clean; `#![deny(unsafe_code)]` holds.
- A `#[test]` per representative formatter feeds a hand-built `Value` and asserts the printed line(s):
  at least `ping`, `render-stats`, `list-entities`, `list-components`, `help` (the two-column table),
  `raycast` (both hit and no-hit branches), `get-selection` (selected vs none), `get-asset-model` (the
  indented bone tree + clip lines), `play`/`get-play-state`, `physics-state`, `get-thumbnail` (the
  base64-length → byte-count math). Each asserts a missing field reads its default without panicking.
- A `#[test]` proves the fallback: an unrecognized command name prints `to_string_pretty(&result)` and a
  non-ASCII string in the result (e.g. an em-dash) appears **unescaped** in text mode.
- A `#[test]` proves `-o json` (phase-1) prints `to_string_pretty(&result)` regardless of command name
  (the `match` is not consulted).
- A `#[test]` for `profiler.capture-stop` with an inline `chromeTrace` (no `path`) writes
  `<temp_dir>/saffron-profile.json` with the trace bytes and prints that path; with a `path` present, it
  prints the path and writes nothing. (Use a per-test temp dir so the test is hermetic.)
- A `#[test]` for `get-asset-model` with a cyclic `parent` index terminates (the 256-guard) and does not
  hang.
