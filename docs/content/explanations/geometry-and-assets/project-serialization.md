+++
title = 'Project files'
weight = 9
+++

# Project files

A project is the asset catalog plus the scene, and both save to one file: `project.json`.
`saveProject` writes them together, `loadProject` replaces them together, and a legacy
two-file layout is migrated on first load. Tying them into one document means a saved
project always has the catalog its scene's UUIDs resolve against.

## One document, two halves

```cpp
inline constexpr int ProjectVersion = 1;

auto saveProject(AssetServer& assets, ComponentRegistry& reg, Scene& scene,
                 const std::string& path) -> Result<void>
{
    nlohmann::json doc;
    doc["version"] = ProjectVersion;
    doc["assets"]  = catalogToJson(assets.catalog);
    doc["scene"]   = sceneToJson(reg, scene);
    // write doc to path...
}
```

`catalogToJson` serializes every `AssetEntry` as `{id, name, type, path}`, with the type
written as a string (`"mesh"`/`"texture"`/`"other"`) so the file is readable and stable
across enum reordering. `sceneToJson` is the registry-driven scene serializer. The document
carries a `version` field checked on load; an unrecognized version is an `Err` rather than a
best-effort parse.

Note what is not saved: the GPU caches and `AssetServer::root`. The catalog stores paths
relative to the root, and the root is set when the server is created, so a project file is
portable across machines as long as the asset directory travels with it.

## Loading replaces both, after a device idle

```cpp
waitGpuIdle(renderer);
assets.meshRefByUuid.clear();
assets.textureRefByUuid.clear();
catalogFromJson(assets.catalog, doc.value("assets", json::array()));
return sceneFromJson(reg, scene, doc.value("scene", json::object()));
```

The ordering matters. The GPU caches hold `Ref`s to meshes and textures the old project
uploaded, and loading a new one must drop them. But a `Ref` dropping frees a `GpuMesh`,
which frees Vulkan buffers that may still be referenced by a frame in flight. So
`loadProject` calls `waitGpuIdle` first, then clears the caches, then swaps the catalog and
scene. With the caches empty, the new scene's UUIDs re-resolve from the new catalog on the
next `renderScene`, [uploading lazily](../asset-server-and-catalog/) as they are first
drawn.

## Legacy migration

Before the unified document there was a separate `asset_registry.json` mapping id → path,
with no names. `newAssetServer` migrates it on construction:

```cpp
entry.id   = Uuid{ std::strtoull(it.key().c_str(), nullptr, 10) };
entry.name = uniqueName(catalog, std::filesystem::path(path).stem().string());
entry.type = type;   // "meshes" => Mesh, "textures" => Texture
putAsset(assets.catalog, std::move(entry));
```

The old file had no human names, so migration synthesizes one from each path's filename stem
and dedups it with `uniqueName`. After migration the catalog lives in `project.json` like
any other. The legacy file is read defensively: anything that is not a string entry under
`meshes`/`textures` is skipped.

## In the code

| What | File | Symbols |
|---|---|---|
| Save the project | `assets.cppm` | `saveProject`, `ProjectVersion` |
| Load the project | `assets.cppm` | `loadProject` |
| Catalog ↔ JSON | `assets.cppm` | `catalogToJson`, `catalogFromJson`, `assetTypeName` |
| Legacy migration | `assets.cppm` | `newAssetServer` |
| Scene half | `scene.cppm` | `sceneToJson`, `sceneFromJson` |

> [!WARNING]
> `loadProject` must `waitGpuIdle` before clearing the caches. Clearing drops the last `Ref`
> to in-flight GPU meshes/textures, freeing their Vulkan buffers; doing that while a frame
> still uses them is a use-after-free. The idle is the ordering guarantee.

## Related

- [Asset catalog](../asset-server-and-catalog/) — what gets serialized
- [Import pipeline](../import-pipeline/) — fills the catalog this persists
- [Scene serialization](../../scene-and-ecs/scene-serialization/) — the `sceneToJson` half
- [Asset commands](../../tooling-and-control/asset-commands/) — `save-project`/`load-project` over the CLI
