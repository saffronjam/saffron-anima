# Phase 2 — Golden / snapshot infrastructure for the byte-exact contracts

**Status:** COMPLETED

**Depends on:** 13-testing-and-verification:phase-1-test-conventions-and-coverage-map

## Goal

Build the golden-fixture infrastructure that catches the silent byte-drift class: a `.smesh`/`.smat`/
`.sanim` byte that shifts, a std430 offset that moves, or a shm header field that changes. These never
throw and never fail validation — they corrupt a mesh, mis-hash a material for dedup, or tear a frame —
so the only detector is a byte comparison against a fixture generated from the *C++ engine's* output.
This phase lands the harness and the initial fixtures; the per-format snapshot `#[test]`s live in the
owning crates (geometry, assets, rendering, host) and cite this harness from their gates.

Concretely:

- **A `fixtures/golden/` directory** committed to the repo holding C++-generated reference artifacts:
  `cube.smesh`, `cube.sanim`, a populated `.smat`, a baked `.smodel`, and a hexdump of each std430
  struct (`MaterialParamsData`, `InstanceData`, `GpuLight`) plus the shm header layout. Each fixture
  carries a tiny sidecar noting the C++ commit + binary it came from (provenance, not back-compat).
- **A snapshot helper** (`assert_bytes_match_golden(path, &actual)`) that diffs `actual` against the
  committed golden and, on mismatch, prints the first differing offset + a windowed hexdump — the
  failure mode an implementer can act on. An `UPDATE_GOLDEN=1` env regenerates fixtures (used only to
  *seed* from C++, never to paper over a real Rust drift).
- **A std430 layout-assertion pattern**: `const _: () = assert!(size_of::<T>() == N)` co-located with
  each `#[repr(C)]` GPU struct (these live in `06-rendering`), plus a `#[test]` that hexdumps a
  known-valued instance and matches the committed golden offset map — catching a field reorder that
  keeps the size but moves an offset.
- **The shm ABI fixture**: the exact header byte layout (magic/version/dims/stride/ring-slot count +
  the per-slot seqlock layout) captured as a golden, matched by a `#[test]` in `08-host-and-viewport`'s
  publisher — the static half of the shm-ABI gate (the live half is host phase 3 against the real
  reader).

This phase produces the *infrastructure and fixtures*; the format-owning phases (`02-math-and-geometry`
phase 3/4/7, `07-assets-and-materials` phase 2/3, `06-rendering` phase 3, `08-host-and-viewport`
phase 2) each add their snapshot test citing it.

## Why this shape (NO LEGACY)

- **The C++ "golden" was prose, not bytes.** The geometry self-test logged vertex/index counts
  (`geometry.cppm:2194`) and the container self-test round-tripped in memory (`:2024`) — neither pinned
  the *bytes on disk*. The feasibility study calls the `.smesh`/`.sanim`/`.smodel` structs a "triple
  contract (disk == container payload == GPU vertex buffer)" that "fails silently if it drifts." A
  byte-exact golden is the correct detector and did not exist before; we add it, we do not port a weaker
  check.
- **Fixtures are generated *from C++*, once, then frozen.** The whole point of the rewrite's
  byte-compatibility requirement (frozen formats so the editor and existing projects load unchanged) is
  that the Rust writer must emit the same bytes the C++ writer did. Seeding the golden from C++ output
  makes that requirement *testable*; after seeding, `UPDATE_GOLDEN` is for an *intentional* format
  change (which, under NO LEGACY, updates the one writer and the one fixture together), not for masking
  drift.
- **std430 size *and* offset both matter.** `MaterialParamsData` is hashed by raw bytes for dedup
  (`06-rendering` README §3); a size-equal field swap silently mis-deduplicates. A size `static_assert`
  alone (the C++ approach) misses this, so the golden offset map is the stronger contract.
- **One harness, used by four areas.** Rather than each format owner writing its own diff, the snapshot
  helper is shared (in `saffron-test-support` from phase 1), so the failure output and the
  `UPDATE_GOLDEN` discipline are identical everywhere.

## Grounding (real files/symbols)

- The byte formats and their writers (golden sources): `engine-old/source/saffron/geometry/geometry.cppm`
  — `.smesh`/`.sanim` writers exercised by `runContainerSelfTest` (`:2024`) and the `.sanim` save/load
  in `runGeometrySelfTest` (`:2249`); `engine-old/source/saffron/assets/assets.cppm` — `.smat` serde and
  the `.smodel` container exercised by `runBakeModelSelfTest` (`:4531`) / `runContainerMetadataSelfTest`
  (`:754`).
- The std430 structs hashed by raw bytes: `engine-old/source/saffron/rendering/renderer_types.cppm` —
  `MaterialParamsData`, `InstanceData`, `GpuLight` (cited in `06-rendering` README §3 grounding row).
- The shm seqlock header: the publisher in `engine-old/source/saffron/host/host.cppm` and its byte
  layout (the contract `editor/src-tauri/src/wayland_viewport.rs` reads — the byte-exact oracle named in
  pre-plan PP-10).
- `tests/e2e/imggen.ts` — the existing in-test PNG codec (`makePng`, the CRC table, the decode path):
  the precedent for "compare buffers directly, no image-diff dep" that the golden image checks reuse.

## Acceptance gate

- `fixtures/golden/` exists with C++-generated `cube.smesh`, `cube.sanim`, a `.smat`, a `.smodel`, the
  three std430 offset maps, and the shm header layout, each with a provenance sidecar.
- `assert_bytes_match_golden` is in `saffron-test-support`, prints first-differing-offset + a hexdump
  window on mismatch, and honors `UPDATE_GOLDEN=1` to (re)seed.
- At least one consuming snapshot `#[test]` is wired and green: a Rust `Mesh` → `.smesh` bytes matches
  `fixtures/golden/cube.smesh` (this also serves as `02-math-and-geometry` phase 3's golden gate).
- A std430 `#[test]` hexdumps a known-valued `MaterialParamsData` and matches the committed offset map.
- `cargo test --workspace` green; clippy + fmt clean; the Cargo workspace compiles.
