# Thumbnail forest rendering

**Status:** NOT STARTED
**Depends on:** phase-1-scene-substrate

## Goal

Make the asset-browser thumbnail show the **assembled** model, not the first node's geometry. Today a
multi-node forest (S2/S4/S5) thumbnails as a fragment — and only that one chunk's material slots are
resolved, so it can render untextured/mis-textured too.

## Crack #7

`engine/crates/assets/src/thumbnail.rs` `build_embedded_job` resolves a `Model` thumbnail by
`model.meta.sub_assets.iter().find(|s| s.asset_type == AssetType::Mesh)` — the **first** mesh sub-asset
only — then `chunk_source_for` reads exactly that chunk into `mesh_bytes`. The importer emits **one mesh
sub-asset per mesh-bearing forest node** (`import.rs:411` onward), so `generate_thumbnail`'s `Model` arm
uploads + renders node 0 alone, and `encode_model_thumbnail_png` frames from that one chunk's AABB
(`thumbnail_render.rs:353`). Sibling node meshes are never uploaded, rendered, or unioned into the bounds.

Two failure modes in one symbol:
- geometry: only the first node renders;
- shading: per-submesh material slots are resolved only for that one chunk.

Also note the degenerate-first edge (audit critic #5): if the first-by-order mesh chunk is empty,
`source.is_empty()` errors out even when later chunks have geometry.

## Fix

Build the thumbnail job over **all** mesh chunks of the container, not the first:

- `build_embedded_job`: collect every `AssetType::Mesh` sub-asset (with its node transform) and the full
  container material table; carry them in `ThumbnailContent::Model`.
- `generate_thumbnail` `Model` arm: upload every chunk and render them at their node-local transforms (the
  thumbnail scene is the same forest the live spawn builds — consider reusing `instantiate_model` +
  `model_render_bounds` so the thumbnail and the in-scene render agree by construction, rather than a
  parallel single-chunk path).
- Framing: union all chunk AABBs (use `model_render_bounds` / the same forest bounds as phase 2) instead
  of `mesh_bounds(first_chunk)`.
- Skip empty chunks rather than erroring on a non-empty model.

This is the largest of the minor cracks; if reusing `instantiate_model` for the thumbnail scene is too
invasive, the minimum is: iterate all mesh chunks, upload+draw each at its node transform, union bounds.

The audit confirmed material-slot ordering is already container-wide and correct
(`gltf_import.rs` shares one `material_table` across nodes), so once all chunks render, their slots map
correctly — no separate material-mapping fix is needed.

## Verify

- Generate a thumbnail for the GothicCommode (S2) via `get-thumbnail`/`view-asset` and assert the encoded
  PNG is non-trivially different from the first-chunk-only render (e.g. content hash differs, or a pixel
  check shows the door geometry present). At minimum, assert no error for a multi-chunk model whose first
  chunk is empty.
- Manual: the `dev` project's asset browser shows the full commode tile, not a fragment.
- `just engine` + `just prepare-for-commit` green.
