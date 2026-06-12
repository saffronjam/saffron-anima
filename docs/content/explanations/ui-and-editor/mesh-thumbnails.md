+++
title = 'Mesh thumbnails'
weight = 11
+++

# Mesh thumbnails

A mesh thumbnail is a small rendered 3/4 preview of an asset, drawn from the mesh itself rather than a
generic icon. The mesh is rendered once into a tiny offscreen image with a camera auto-framed to its
bounds, read back as a base64 PNG, and shown in the webview as an `<img>`. The render is engine-side;
the transport is the control socket.

## How it works

The preview camera is placed from the mesh's bounding box so any mesh fills the frame regardless of
size. It finds the center and a bounding radius, then backs the camera off by the distance that fits
that radius in the field of view:

```cpp
const glm::vec3 center = (mesh->boundsMin + mesh->boundsMax) * 0.5f;
f32 radius = glm::length(mesh->boundsMax - mesh->boundsMin) * 0.5f;
if (radius <= 0.0001f) { radius = 1.0f; }
const f32 fovy = glm::radians(45.0f);
const f32 distance = radius / glm::tan(fovy * 0.5f) * 1.3f;
const glm::vec3 eye = center + glm::normalize(glm::vec3(1.0f, 0.7f, 1.0f)) * distance;
```

The eye offset `(1, 0.7, 1)` gives the canonical 3/4 view. The `1.3` factor leaves a margin so the
mesh does not touch the edges, and the degenerate-radius guard keeps a flat or point mesh from putting
the camera on top of it. Near and far planes are derived from the framing distance for a tight depth
range, and the projection's Y is flipped to match the viewport convention so the thumbnail comes out
upright.

## A one-shot render

The thumbnail pipeline is deliberately bare: vertex position, normal, and uv in; a two-matrix push
constant (MVP and a normal matrix); no descriptor sets, no lighting, no materials. The mesh shows in a
flat color. The render is multisampled at the highest count the device supports (up to 8x) — at
thumbnail sizes geometry edges alias hard without it, and a one-shot tiny render makes the extra
samples free in practice. The pass draws into a transient MSAA target and resolves into the 1x image
that gets read back; the sample count is independent of the viewport's [AA mode](../../anti-aliasing/aa-modes/).
The image is rendered with a one-time-submit command buffer through dynamic rendering — clear to dark
gray, draw each submesh, resolve, then transition for the readback:

```cpp
transitionImage(cmd, color.image, eUndefined, eColorAttachmentOptimal, ...);
cmd.beginRendering(rendering);
cmd.bindPipeline(eGraphics, renderer.pipelines.thumbnail->pipeline);
cmd.pushConstants(... , &push);
cmd.bindVertexBuffers(0, mesh->vertexBuffer, offset);
cmd.bindIndexBuffer(mesh->indexBuffer, 0, eUint32);
for (const Submesh& submesh : mesh->submeshes)
    cmd.drawIndexed(submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
cmd.endRendering();
```

The render is synchronous — submit, then `waitIdle`. Thumbnails are built lazily and once, between
frames on the [control-drain step, off the present path](../../tooling-and-control/asset-commands/),
so the wait is acceptable. The pipeline is built on first use and cached on the renderer
(`renderer.pipelines.thumbnail`), so the second thumbnail reuses it. A texture asset skips the render
and copies its decoded image straight back.

## Model thumbnails are textured

A bare mesh shows in a flat color, but a [`.smodel`](../../geometry-and-assets/smodel-container/) tile
shows the model as it looks — its mesh shaded with the embedded materials. `renderModelThumbnail`
frames the mesh exactly as above, then draws each submesh through the **material-preview pipeline**
(`preview.slang`, the same studio-lit shader the [material preview](../../materials-and-pipelines/native-materials/)
uses) with that submesh's material from the table, indexed by `Submesh.materialSlot`:

```cpp
for (const Submesh& submesh : mesh->submeshes) {
    const SubmeshMaterial& m = submeshMaterials[min(submesh.materialSlot, submeshMaterials.size() - 1)];
    push.baseColor = m.baseColor;
    push.tex = { idx(m.albedoTexture), idx(m.metallicRoughnessTexture), idx(m.normalTexture), features };
    push.pbr = { m.metallic, m.roughness, m.normalStrength, 0 };
    cmd.pushConstants(...); cmd.drawIndexed(submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
}
```

The materials and their textures live as chunks of the container, so the thumbnail worker has no
`AssetServer`: the main thread resolves them at enqueue — the mesh chunk into `meshBytes`, each material
into a `MaterialAsset`, and each referenced texture's chunk into `materialTextures` bytes — and the
worker decodes from memory, uploads, and builds the `SubmeshMaterial` table. Bumping
`ThumbnailCacheVersion` retires the older flat-rendered model thumbnails so they regenerate textured.

## Across the socket as a PNG

There is no shared GPU context with the webview. The rendered image is read back into a host-visible
staging buffer, encoded to a PNG in memory, and returned as base64 in the `get-thumbnail` result
(`{format, width, height, base64}`, the dimensions truthful to the PNG). The [Assets
panel](../assets-panel-and-thumbnails/) asks for 128px; the View modal re-renders at 512px through
`view-asset`. A mesh renders straight into the requested size, so it needs no downscale before
readback — that step only kicks in for an oversized [texture](../assets-panel-and-thumbnails/) asset.

The render is not synchronous on the request. A cold miss is generated on the engine's thumbnail
[worker thread](../assets-panel-and-thumbnails/) (the loaded mesh is handed back to the main thread's
cache afterward): `get-thumbnail` replies `pending` and the result lands in the persistent disk cache,
so the editor's retry — and every later start — is a plain cache read, never a frame-loop stall.

The webview decodes the base64 to a `Blob`, makes an object URL, and caches it by asset id. The
readback runs once per asset, not once per tile or per frame. That
[blob-URL cache](../assets-panel-and-thumbnails/) is where the size-reuse and de-dup live.

## In the code

| What | File | Symbols |
|---|---|---|
| The render | `renderer_thumbnail.cpp` | `renderMeshThumbnail` |
| Textured model render | `renderer_thumbnail.cpp`; `assets.cppm` | `renderModelThumbnail`, the `Model` job in `generateThumbnail` |
| Auto-framing | `renderer_thumbnail.cpp` | `center`/`radius`/`distance`, the `(1, 0.7, 1)` eye |
| The minimal pipeline | `renderer_thumbnail.cpp` | `newThumbnailPipeline`, `renderer.pipelines.thumbnail` |
| MSAA + resolve | `renderer_thumbnail.cpp` | `thumbnailSampleCount`, the `resolveMode` color attachment |
| Readback → base64 PNG (engine) | `control_commands_asset.cpp` | `get-thumbnail`, `view-asset` |
| Decode + blob-URL cache (client) | `editor/src/state/store.ts` | `getThumbnailUrl`, `base64ToBlob`, `thumbnailCache` |

## Related

- [Assets panel & thumbnails](../assets-panel-and-thumbnails/) — where the preview is shown + cached
- [Asset commands](../../tooling-and-control/asset-commands/) — the `get-thumbnail`/`view-asset` readback
- [Mesh and GPU upload](../../geometry-and-assets/) — the `GpuMesh` bounds the framing reads
