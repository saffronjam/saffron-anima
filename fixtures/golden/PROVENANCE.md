# Golden fixtures — provenance

These are byte-exact reference artifacts for the frozen on-disk and GPU-upload formats.
They are the detector for the **silent byte-drift class**: a `.smesh`/`.smat`/`.sanim` byte
that shifts, an std430 offset that moves, or a shm header field that changes. None of those
throw or fail validation — they corrupt a mesh, mis-hash a material for dedup, or tear a
frame — so the only detector is a byte comparison against a fixture generated from the C++
engine's output.

## How they were generated (not back-compat)

Every fixture was emitted **once** by `gen/gen_golden.cpp` — a standalone C++ program whose
writer logic is transcribed **verbatim** from the C++ engine's format owners (the disk
formats are pure `#[repr(C)]`/JSON data, independent of Vulkan/Jolt/SDL, so the generator
needs none of them). The `.smat`/META JSON is produced with the same `nlohmann::json` the
engine vendors, so its f64-promoted float formatting and `std::map` sorted keys match the
engine byte-for-byte.

| Fixture | Format owner (C++) | Symbol |
|---|---|---|
| `cube.smesh` | `engine-old/source/saffron/geometry/geometry.cppm` | `encodeMeshImage` / `SMeshHeader` (`:386`, `:1400`) |
| `cube.sanim` | `geometry.cppm` | `saveAnimationToBuffer` / `SANimHeader` / `SANimTrackRecord` (`:406`, `:1619`) |
| `cube.smodel` | `geometry.cppm` | `writeContainer` / `SModelHeader` / `TocEntry` (`:296`) |
| `material.smat` | `engine-old/source/saffron/assets/assets.cppm` | `materialAssetToJson` + `.dump(2)` (`:1488`, `:2137`) |
| `instance_data.offsets` | `engine-old/source/saffron/rendering/renderer_types.cppm` | `InstanceData` (`:1868`) |
| `material_params_data.offsets` | `renderer_types.cppm` | `MaterialParamsData` (`:1884`) |
| `gpu_light.offsets` | `renderer_types.cppm` | `GpuLight` (`:2018`) |
| `shm_header.layout` | `engine-old/source/saffron/rendering/renderer_capture.cpp` | `recreateShmSegment` header init (`:129`) |

- **Source tree commit:** `d8b4cea` (the `feat/to-rust` worktree at fixture-generation time;
  `engine-old/` is the verbatim C++ reference the generator transcribes).
- **nlohmann/json:** `v3.12.0` (the engine's `cmake/Dependencies.cmake` pin) — the `.smat`
  and `.smodel` META float/key formatting is reproduced from this exact version.
- **Compiler:** clang++ 21 (`-std=c++26`) in the `saffron-build` toolbox.

The byte equality is independently corroborated: the C++ engine's struct layouts are pinned
by `static_assert(sizeof(...) == N)` and the Rust ports carry the matching
`const _: () = assert!(size_of::<T>() == N)` + `offset_of!` unit tests, so the snapshot
fixtures and the per-struct layout asserts agree from two directions.

## Regenerating (seed / intentional change only)

```sh
toolbox run -c saffron-build bash -lc '
  cd <repo>
  INC=build/debug/_deps/nlohmann_json-src/single_include   # the engine's vendored v3.12.0
  clang++ -std=c++26 -I"$INC" -o /tmp/gen_golden fixtures/golden/gen/gen_golden.cpp
  /tmp/gen_golden fixtures/golden
'
```

The Rust side reseeds the *same bytes* with `UPDATE_GOLDEN=1 cargo test` (it writes `actual`
into the fixture instead of asserting). Use either path **only** to seed the fixtures the
first time or to land an *intentional* format change — under NO LEGACY that change updates
the one writer and the one fixture together, in the same commit. A reseed is never the way
to quiet a real Rust drift: a drift is a finding, not a reseed.

## The consuming snapshot `#[test]`s

The per-format snapshots live in the owning crates and cite `assert_bytes_match_golden`
(`saffron-test-support`):

| Fixture | Consuming test |
|---|---|
| `cube.smesh` / `cube.sanim` / `cube.smodel` | `engine/crates/geometry/tests/golden_snapshot.rs` |
| `*.offsets` | `engine/crates/rendering/tests/std430_golden_snapshot.rs` |
| `material.smat` | `engine/crates/assets/tests/smat_golden_snapshot.rs` |
| `shm_header.layout` | `engine/crates/host/tests/shm_header_golden.rs` |
