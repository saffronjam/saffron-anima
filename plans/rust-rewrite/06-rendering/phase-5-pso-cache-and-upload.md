# Phase 5 — mesh/texture upload + the übershader PSO cache

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-4-bindless-and-samplers, saffron-geometry

## Goal

Port the upload paths and the pipeline-state-object cache. `upload_mesh` (with the optional `VertexSkin`
second stream) and `upload_texture`/`upload_texture_float` produce the `Arc<GpuMesh>`/`Arc<GpuTexture>`
the asset layer and scene draw use. `request_mesh_pipeline` is the PSO cache front door: one übershader
backs every renderable, and a variant flag (unlit / skinned / wireframe) selects a cached PSO. This is
where the std430 `MaterialParamsData` contract (README §3) first becomes load-bearing.

## Why this shape (NO LEGACY)

- **One übershader, a small permutation set, a cache keyed by variant — not a PSO per material.** The
  mesh PSO is the lit or unlit übershader (`renderer_pipelines.cpp:52`), plus skinned (binds
  `vertexMainSkinned` + the `VertexSkin` stream on binding 1, `:76`) and wireframe
  (`vk::PolygonMode::eLine`, gated on `fill_mode_non_solid`). The cache is a
  `HashMap<PsoKey, Arc<Pipeline>>` keyed by the variant tuple; `request_mesh_pipeline` builds-and-caches
  on first request and returns the shared `Arc`. The C++ keys by a string; the Rust port uses a typed
  `PsoKey` (`{ unlit, skinned, wireframe, sample_count }`) so the key is matchable, not a stringly-typed
  concat — one cache, one key shape.
- **`MaterialParamsData` is `#[repr(C)]` + `bytemuck::Pod` with `const _: () = assert!(size_of == 96)`.**
  This struct is hashed by raw bytes for per-frame material dedup (phase 6), so a wrong offset corrupts
  dedup, not just a pixel. The layout is pinned exactly: `baseColor`/`pbr`/`emissive`/`uv` vec4s, `tex0`/
  `tex1` uvec4s (bindless indices + feature bits) — see the C++ `static_assert`
  (`renderer_types.cppm:1893`).
- **Uploads return `Arc<T>` (read-shared assets), never `Arc<Mutex>`.** A `GpuMesh`/`GpuTexture` is built
  once then only read; the default `Ref = Arc` bucket applies (refPolicy bucket 1). The submit/present
  path takes the `gpuQueueMutex` for the staging-copy submit (README §5) since the worker thread also
  uploads.
- **`upload_texture_float` narrows f32→f16 on the CPU before staging** (HDR panoramas / env sources),
  matching the C++ (`renderer.cppm:2 uploadTextureFloat` doc). The `image`/stb decode happens in
  `saffron-geometry`; this phase consumes decoded bytes.
- **The lazy thumbnail/preview pipelines + the preview sphere are created on first use, with the
  prewarm hook for the worker thread** (`prewarmThumbnailResources`, `renderer_types.cppm:1977`) so the
  worker never races their init — the same idempotent-prewarm contract.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer_pipelines.cpp` — `requestMeshPipeline` (`:205`/`:238`),
  the lit/unlit/skinned/wireframe variants (`:52`/`:76`), the cache.
- `engine-old/source/saffron/rendering/renderer_drawlist.cpp` — `uploadMesh` (both forms, the skin
  stream).
- `engine-old/source/saffron/rendering/renderer_textures.cpp` — `uploadTexture`, `uploadTextureFloat`.
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `Material` (`:549`), `SubmeshMaterial`
  (`:557`), `MaterialParamsData` + `static_assert == 96` (`:1884`/`:1893`), `Pipelines` (`:1227`),
  `newMeshPipeline`/`requestMeshPipeline`/`pipelineCount` (`:1898`–`:1907`),
  `prewarmThumbnailResources` (`:1977`).
- `engine-old/source/saffron/geometry/geometry.cppm` — `Vertex` (`:36`, 32B), `VertexSkin` (`:63`, 24B)
  the PSO's vertex-input layouts must match.
- README §3 (the std430 / bytemuck contract).

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - `size_of::<MaterialParamsData>() == 96` and the field byte offsets match the C++ layout (a const
    assert + an offset test); `InstanceData` and `GpuLight` offset asserts land here too.
  - `request_mesh_pipeline` for the same variant twice returns the *same* `Arc` (cache hit, one PSO);
    distinct variants produce distinct entries; `pipeline_count` reflects the cache size (proves
    übershader reuse — many materials, few PSOs).
  - uploading a mesh with a skin stream produces a `GpuMesh` with a non-null skin buffer; without, null.
- A validation-clean GPU smoke uploads a mesh + a texture and draws it once with the cached PSO, zero
  validation messages.
