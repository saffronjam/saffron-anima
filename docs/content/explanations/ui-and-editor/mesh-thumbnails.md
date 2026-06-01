+++
title = 'Mesh thumbnails'
weight = 11
+++

# Mesh thumbnails

A mesh in the [Assets panel](../assets-panel-and-thumbnails/) shows a small rendered 3/4 preview of itself, not a generic icon. `renderMeshThumbnail` draws the mesh once into a tiny offscreen image with a camera auto-framed to its bounds; the engine then reads that image back as a base64 PNG and the webview shows it as an `<img>`. The render is engine-side; the transport is the control socket.

## Auto-framing the mesh

The preview camera is placed from the mesh's bounding box so any mesh fills the frame regardless of size. It finds the center and a bounding radius, then backs the camera off by the distance that fits that radius in the field of view:

```cpp
const glm::vec3 center = (mesh->boundsMin + mesh->boundsMax) * 0.5f;
f32 radius = glm::length(mesh->boundsMax - mesh->boundsMin) * 0.5f;
if (radius <= 0.0001f) { radius = 1.0f; }
const f32 fovy = glm::radians(45.0f);
const f32 distance = radius / glm::tan(fovy * 0.5f) * 1.3f;
const glm::vec3 eye = center + glm::normalize(glm::vec3(1.0f, 0.7f, 1.0f)) * distance;
```

The eye offset `(1, 0.7, 1)` gives the canonical 3/4 view. The `1.3` factor leaves a margin so the mesh doesn't touch the edges, and the degenerate-radius guard keeps a flat or point mesh from putting the camera on top of it. Near/far planes are derived from the framing distance for a tight depth range, and the projection's Y is flipped to match the viewport convention so the thumbnail comes out upright.

## A one-shot render

The thumbnail pipeline is deliberately bare: vertex position/normal/uv in, a two-matrix push constant (MVP and a normal matrix), no descriptor sets, no lighting, no materials. The mesh shows in a flat color. The image is rendered with a one-time-submit command buffer through dynamic rendering — clear to dark gray, draw each submesh, then transition to transfer-source for the readback:

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

This is synchronous — submit, then `waitIdle` — which is fine because thumbnails are built lazily and once, between frames on the [control-drain step, off the present path](../../tooling-and-control/asset-commands/). The pipeline is built on first use and cached on the renderer (`renderer.pipelines.thumbnail`), so the second thumbnail reuses it. A texture asset skips the render and copies its decoded image straight back.

## Across the socket as a PNG

There is no shared GPU context with the webview, so the rendered image is read back into a host-visible staging buffer, encoded to a PNG in memory, and returned as base64 in the `get-thumbnail` result (`{format, size, base64}`). The Assets panel asks for 128px; the [View modal](../assets-panel-and-thumbnails/) re-renders at 512px through `view-asset`.

The webview decodes the base64 to a `Blob`, makes an object URL, and caches it by asset id — so the readback runs once per asset, not once per tile or per frame. That [blob-URL cache](../assets-panel-and-thumbnails/) is where the size-reuse and de-dup live.

## In the code

| What | File | Symbols |
|---|---|---|
| The render | `renderer_thumbnail.cpp` | `renderMeshThumbnail` |
| Auto-framing | `renderer_thumbnail.cpp` | `center`/`radius`/`distance`, the `(1, 0.7, 1)` eye |
| The minimal pipeline | `renderer_thumbnail.cpp` | `newThumbnailPipeline`, `renderer.pipelines.thumbnail` |
| Readback → base64 PNG (engine) | `control_commands_asset.cpp` | `get-thumbnail`, `view-asset` |
| Decode + blob-URL cache (client) | `editor/src/state/store.ts` | `getThumbnailUrl`, `base64ToBlob`, `thumbnailCache` |

## Related

- [Assets panel & thumbnails](../assets-panel-and-thumbnails/) — where the preview is shown + cached
- [Asset commands](../../tooling-and-control/asset-commands/) — the `get-thumbnail`/`view-asset` readback
- [Mesh and GPU upload](../../geometry-and-assets/) — the `GpuMesh` bounds the framing reads
