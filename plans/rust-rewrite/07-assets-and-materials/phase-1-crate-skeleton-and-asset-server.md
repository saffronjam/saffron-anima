# Phase 1 — crate skeleton and `AssetServer`

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-2-core-crate, 00-foundations:phase-4-json-crate, 03-ecs-and-scene (AssetCatalog types), 06-rendering:phase-3-gpu-resources (GpuMesh/GpuTexture Drop)

## Goal

Stand up `saffron-assets` as a workspace lib crate (`#![deny(unsafe_code)]`) with the `AssetServer`
struct, the three uuid-keyed GPU caches as the negative-cache (`HashMap<u64, Option<Arc<T>>>`), the
generic get-or-negative-cache helper, the reserved-id sentinels, the typed `Error`/`Result`, and the
`clear_asset_caches` path that relies on `Arc`+`Drop` ordering. No import, no materials, no
`render_scene` yet — this phase is the cache container and its lifetime discipline.

## Why this shape (NO LEGACY)

The negative-cache is the explicit feasibility callout and the contract that fails silently if it
drifts; it gets locked first, in isolation, with its own tests, before any loader populates it. The C++
expresses "present-but-null = negative marker" via `find() != end()` returning a null `Ref`; Rust
expresses it precisely as `Option<HashMap::get>` (presence) wrapping `Option<Arc<T>>` (success). The
get-or-negative-cache shape is written once as a generic helper rather than copy-pasted into the five
resolve functions (one code path). The C++ `wait_gpu_idle`-then-`clearAssetCaches` ordering survives as
a call-site discipline, but the *freeing* is `Drop` on the last `Arc`, not a manual destroy loop.

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `AssetServer` (the struct with `root`, `catalog`,
  `meshRefByUuid`, `textureRefByUuid`, `modelRefByUuid`, `editorCameraModel`, `thumbnailWorker`),
  `clearAssetCaches`, `newAssetServer`, `setAssetRoot`, `ensureAssetDirectories`, the negative-cache rule
  documented at the struct (`a cached null Ref is the negative-cache marker`), `DefaultMaterialId{1}`,
  `PreviewFloorMeshId{2}`, `SystemMeshVisual`.
- The AGENTS rule: "A cached `null` `Ref` is a negative-cache marker, not a miss" and "Clear caches only
  after `waitGpuIdle`" (`engine-old/source/saffron/assets/AGENTS.md`).
- Upstream: `AssetCatalog` (`engine-old/source/saffron/scene/scene.cppm`), `GpuMesh`/`GpuTexture`
  (`engine-old/source/saffron/rendering/renderer_types.cppm`, move-only RAII Drop wrappers).

## Acceptance gate

- `cargo build -p saffron-assets` compiles with `#![deny(unsafe_code)]`; workspace stays green; clippy +
  fmt clean.
- `#[test]`s: the generic `resolve_cached` returns a cached `Some(arc)`, returns a cached `None` *without
  re-invoking the loader* (negative-cache: assert the loader closure ran zero times on the second call),
  and invokes the loader exactly once on an absent key.
- A `#[test]` proves `clear_asset_caches` drops all three caches and that dropping the last `Arc<T>` of a
  stub GPU resource runs its `Drop` (a counting stub asserts the destroy fired exactly once); a reserved
  sentinel seeded into the mesh cache survives a `get` but is dropped by `clear_asset_caches`.
- `AssetServer::new(root)` seeds the root + an empty catalog; `set_asset_root` + `ensure_asset_directories`
  create `models/`, `textures/`, `materials/`, `cache/thumbnails/` (assert the dirs exist).
