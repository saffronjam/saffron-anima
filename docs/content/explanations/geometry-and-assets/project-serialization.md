+++
title = 'Project files'
weight = 9
+++

# Project files

A project is a folder with one `project.json` and a project-local `assets/` directory. A
scene refers to meshes and textures by UUID; the catalog in `project.json` maps those UUIDs
to files under the project's asset root. Keeping both in one project folder means the editor
can copy or archive a project without depending on repo-root runtime assets.

During local development, app data lives under repo-root `appdata/`, with user projects under
`appdata/userdata/<project-name>/`. Packaged builds can swap that base directory behind the
same app-data helper.

```text
appdata/userdata/<project-name>/
  project.json
  assets/
    models/
    textures/
  src/            Lua scripts (ScriptComponent slot paths resolve here)
```

The `src/` folder is ensured on create and on load (with a starter `example.lua` when absent);
its contents are plain files, never catalog entries — see
[Script components](../../scripting/script-components-and-runtime/).

The project name is the stable folder-safe id. `displayName` is stored separately in the
project file and is what the editor shows to users.

## How it works

A project save serializes one JSON document: a version, the project name, the display name,
the asset catalog, the scene, the renderer settings, and the editor camera. The catalog lists every asset by id,
name, type, and path. The scene half is the registry-driven scene serializer. A load reverses
this, after first making the GPU idle so the previous project's resources can be released safely.

## One document

```json
{
  "version": 1,
  "name": "sample-project",
  "displayName": "Sample Project",
  "assets": [
    {
      "id": 3862017159553017004,
      "name": "cube",
      "type": "mesh",
      "path": "models/3862017159553017004.smesh"
    }
  ],
  "scene": {
    "version": 2,
    "entities": []
  },
  "renderSettings": {
    "aa": "msaa4",
    "exposureEv": 0.0,
    "clustered": true,
    "shadows": true
  },
  "editorCamera": {
    "position": { "x": 3.0, "y": 2.5, "z": 4.0 },
    "yaw": -37.0,
    "pitch": -29.0,
    "fov": 45.0
  }
}
```

`catalogToJson` serializes every `AssetEntry` as `{id, name, type, path}`. The type is written
as a string (`"mesh"`/`"texture"`/`"other"`), so the file stays readable and stable across enum
reordering. `sceneToJson` is the registry-driven scene serializer. The `version` field is checked
on load; an unrecognized version is an `Err` rather than a best-effort parse.

Two things are deliberately not saved: the GPU caches and the absolute `AssetServer::root`.
The catalog stores paths relative to `<project-root>/assets`, and the root is set when the
project opens.

## Render settings ride along

`renderSettings` captures the renderer state the editor's render panel drives — the
[AA mode](../../anti-aliasing/aa-modes/), tonemap exposure, and the feature toggles
(clustered, depth prepass, shadows, IBL, SSAO, contact shadows, SSGI, DDGI, RT shadows,
ReSTIR) — so a project reopens looking the way it was saved. The block is applied through
the same setters the control commands use; missing fields keep their current value, so a
project saved before the block existed loads unchanged, and the RT toggles only apply on
a device that reports ray-tracing support.

The [editor camera](../../ui-and-editor/editor-camera/) rides along the same way:
`editorCamera` stores the viewport eye (position, yaw, pitch, fov), so a reopened project
shows the framing it was saved with. The serde lives in SceneEdit — the camera is editor
state, not an asset — and `saveProject`/`loadProject` just carry the block; callers in the
control commands and the host startup path apply it. A project without the block keeps the
current camera, like the render settings.

## Loading replaces both, after a device idle

```cpp
waitGpuIdle(renderer);
assets.meshRefByUuid.clear();
assets.textureRefByUuid.clear();
catalogFromJson(assets.catalog, doc.value("assets", json::array()));
return sceneFromJson(reg, scene, doc.value("scene", json::object()));
```

The ordering matters. The GPU caches hold `Ref`s to meshes and textures the old project
uploaded, and loading a new one must drop them. Dropping a `Ref` frees a `GpuMesh`, which frees
Vulkan buffers that may still be referenced by a frame in flight. So `loadProject` calls
`waitGpuIdle` first, then clears the caches, then swaps the catalog and scene. With the caches
empty, the new scene's UUIDs re-resolve from the new catalog on the next `renderScene`,
[uploading lazily](../asset-server-and-catalog/) as they are first drawn.

## Startup and commands

The Tauri editor owns startup project choice. If `SAFFRON_PROJECT` is set, the engine opens
that project immediately. The value can be a project name under app data, a project directory,
or a direct `project.json` path. `SAFFRON_AUTO_EMPTY_PROJECT=1` creates an empty test project
without showing the startup modal.

The control plane exposes project-aware commands:

- `get-project` returns the active project state.
- `new-project` creates and opens a project.
- `open-project` opens an existing project folder or file.
- `save-project` saves the active project when no path is passed.
- `load-project` remains as a compatibility alias for older tooling.

## Project-local assets

Imported meshes are baked under `assets/models/<uuid>.smesh`. Imported textures are copied
under `assets/textures/<uuid>.<ext>`. `import-model`, `import-texture`, and the cube/model
entity preset require an active project so imports cannot accidentally write into the engine's
runtime asset directory.

## Legacy compatibility

An older `asset_registry.json` mapped id → path, with no names. `newAssetServer` migrates it on
construction:

```cpp
entry.id   = Uuid{ std::strtoull(it.key().c_str(), nullptr, 10) };
entry.name = uniqueName(catalog, std::filesystem::path(path).stem().string());
entry.type = type;   // "meshes" => Mesh, "textures" => Texture
putAsset(assets.catalog, std::move(entry));
```

The old file had no human names, so migration synthesizes one from each path's filename stem
and dedups it with `uniqueName`. After migration the catalog lives in `project.json` like any
other catalog. The legacy file is read defensively: anything that is not a string entry under
`meshes`/`textures` is skipped.

Old project catalogs may also contain mesh paths beginning with `meshes/`. Loading keeps those
paths working. New imports and new saves use `models/`.

## In the code

| What | File | Symbols |
|---|---|---|
| Save the project | `assets.cppm` | `saveProject`, `ProjectVersion` |
| Load the project | `assets.cppm` | `loadProject` |
| Catalog ↔ JSON | `assets.cppm` | `catalogToJson`, `catalogFromJson`, `assetTypeName` |
| Render settings ↔ JSON | `assets.cppm` | `renderSettingsToJson`, `applyRenderSettings` |
| Editor camera ↔ JSON | `scene_edit_camera.cpp` | `sceneEditCameraToJson`, `sceneEditCameraFromJson` |
| Legacy migration | `assets.cppm` | `newAssetServer` |
| Project commands | `control_commands_asset.cpp` | `get-project`, `new-project`, `open-project`, `save-project` |
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
