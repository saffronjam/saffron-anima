# Phase 4 — resolve and load paths (the negative-cache loaders)

**Status:** COMPLETED

**Depends on:** 07-assets-and-materials:phase-3-container-metadata-and-model-open, 02-math-and-geometry (mesh/anim byte codecs + image decode), 06-rendering:phase-5-pso-cache-and-upload (upload_mesh/upload_texture)

## Goal

Port the cache resolve/load functions that fill the negative-cache from geometry's byte codecs + image
decode and rendering's GPU upload: `load_mesh_from_source`, `load_texture_from_source` (colorspace →
upload format), `resolve_mesh`, `resolve_texture`, `resolve_material`, `load_mesh_asset`,
`load_texture_asset`, `load_anim_clip` (the animation runtime's injected loader), `load_mesh_cpu_asset`
(physics cooking), `ensure_preview_floor_mesh`, `load_editor_camera_model`.

## Why this shape (NO LEGACY)

Each loader follows the phase-1 get-or-negative-cache shape: cache hit returns the stored
`Option<Arc<T>>` (live or negative); a miss attempts a load, inserts the result (or `None` on failure +
a one-time warn), and returns it. Every distinct failure mode negative-caches: bytes unreadable, decode
failed, upload failed, dangling catalog id, no such chunk. The colorspace → format mapping is exact:
`Hdr` → `upload_texture_float`; `Linear` → unorm; `Srgb`/`Auto` → sRGB (`srgb = space != Linear`). A
standalone texture's explicit `.smeta` colorspace wins, else the row's `hdr`/`linear` provenance. The
embedded-vs-standalone fork routes through the container (`resolve_*`) or the file path. A dangling
texture id falls back in the draw path to rendering's default-white slot — the loader returns `None`,
the draw path substitutes slot 0; the loader does **not** retry. `load_anim_clip` and
`load_mesh_cpu_asset` are `Result`-returning (not cache-backed) one-shots used by the animation runtime
and physics cooking respectively — they resolve the same embedded/standalone fork but read CPU data.

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `loadMeshFromSource`, `loadTextureFromSource` (the
  `Colorspace::Hdr` → `uploadTextureFloat`, else `decodeImageFromMemory` + `srgb = space != Linear`
  fork), `resolveMesh`, `resolveTexture`, `resolveMaterial`, `loadMeshAsset` (the `meshes/` →
  `models/` path fixup), `loadTextureAsset` (the `.smeta`/`hdr`/`linear` colorspace recovery + the
  dangling-id "using default" warn + negative cache), `loadAnimationClipAsset`, `loadMeshCpuAsset`,
  `ensurePreviewFloorMesh` (reserved `PreviewFloorMeshId`, no catalog row), `loadEditorCameraModel`
  (attempted-once `SystemMeshVisual`).
- Upstream: geometry `load_mesh_from_bytes`/`load_mesh_skin_from_bytes`/`decode_image`/`decode_image_hdr`/
  `mesh_counts_from_bytes`/`load_anim_clip`; rendering `upload_mesh`/`upload_texture`/`upload_texture_float`.
- The negative-cache + default-white-fallback rules in `engine-old/source/saffron/assets/AGENTS.md`.

## Acceptance gate

- `cargo build -p saffron-assets` + workspace green; clippy + fmt clean.
- `#[test]`s (with stub upload returning a counting `Arc`): a successful load caches a live `Arc` and the
  second call reuses it without re-decoding; a decode-failure and an upload-failure each cache `None` and
  do **not** retry on the second call; a dangling catalog id caches `None` and warns once.
- A colorspace `#[test]`: `Hdr` routes to the float uploader, `Linear` uploads non-sRGB, `Srgb`/`Auto`
  upload sRGB; a standalone row's `.smeta` colorspace overrides the `hdr`/`linear` provenance.
- `ensure_preview_floor_mesh` seeds `PreviewFloorMeshId` into the mesh cache without a catalog row;
  `load_editor_camera_model` is attempted exactly once (a failed attempt does not re-translate).
- `load_anim_clip` / `load_mesh_cpu_asset` resolve embedded (via container chunk) and standalone (via
  file) sources and return a typed `Err` (not a panic) on a missing/wrong-type catalog id.
