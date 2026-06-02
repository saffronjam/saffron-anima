+++
title = 'Geometry & assets'
weight = 5
+++

# Geometry & assets

The asset path turns a model on disk into triangles the GPU can draw. Import reads glTF and OBJ
into one common `Mesh` and bakes it to a versioned `.smesh`; the asset server then keys meshes and
textures by UUID, caches their GPU resources, names them in a catalog, and feeds the draw list.

## Pages

| Page | Covers | Code |
|---|---|---|
| `mesh-and-vertex-layout` | `Vertex` (pos/normal/uv), `Mesh`, `Submesh`, fixed stride | `geometry.cppm` · `Vertex`, `Mesh` |
| `gltf-and-obj-import` | cgltf + tinyobjloader through their no-throw APIs into a common mesh | `geometry.cppm` · import fns |
| `smesh-format` | the baked, versioned binary mesh format | `geometry.cppm` · save/load mesh |
| `image-decoding` | stb_image PNG/JPG → RGBA8, embedded glTF textures | `geometry.cppm` · `decodeImage` |
| `gpu-mesh-upload` | VMA staging, `GpuMesh`, mesh AABB bounds | `renderer_drawlist.cpp` · `uploadMesh` |
| `asset-server-and-catalog` | `AssetServer`, UUID→GPU caches, the named/renameable catalog | `assets.cppm` · `AssetServer` |
| `import-pipeline` | `importModel` / `importTexture`, baking, negative caching | `assets.cppm` |
| `draw-list` | `renderScene` → flat `DrawItem` list, `(mesh, albedo)` instanced buckets | `assets.cppm`; `renderer_drawlist.cpp` · `submitDrawList` |
| `project-serialization` | `saveProject`/`loadProject`, the unified `project.json`, legacy migration | `assets.cppm` |

> [!NOTE]
> Per-submesh multi-material (`materialSlot`) is reserved in the data model but not yet
> wired through the draw path. The import and draw-list pages mark where it stops.
