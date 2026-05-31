# Editor icons

SVGs are vendored from [Lucide](https://lucide.dev/icons/) and copied next to the
binary by CMake on every build (`editor/CMakeLists.txt` copies the whole `icons/`
directory to the runtime dir).

## Adding a new icon

```sh
# From the repo root:
curl -fsSL https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/<name>.svg \
     -o editor/assets/icons/<name>.svg
```

Browse available names at https://lucide.dev/icons/ — the URL slug is the filename
without `.svg`.

## Loading an icon at runtime

In `editor/source/main.cpp` `onCreate`, follow the existing pattern:

```cpp
state->myIcon = loadIcon("icons/<name>.svg");
// loadIcon calls:
//   se::uploadSvgIcon(app.renderer, se::assetPath(file), 64, glm::vec4(0.85f))
// size = raster resolution (px); tint = glm::vec4(0.85f) ≈ near-white
```

Store the result in `State` as a `Thumbnail { Ref<GpuTexture> tex; ImTextureID id; }`.
Pass `id` wherever an `ImTextureID` is needed (e.g. `ImGui::Image`, `AddImage`).

## Current icons

| File | Lucide name | Used for |
|---|---|---|
| `box.svg` | box | Mesh asset fallback |
| `image.svg` | image | Texture asset fallback |
| `file.svg` | file | Unknown asset fallback |
| `lightbulb.svg` | lightbulb | PointLight editor billboard |
| `flashlight.svg` | flashlight | SpotLight editor billboard |
| `camera.svg` | camera | Camera editor billboard |
