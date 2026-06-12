# Editor icons (legacy)

SVGs vendored from [Lucide](https://lucide.dev/icons/). CMake still copies the whole
`icons/` directory next to the binary on every build (`engine/CMakeLists.txt`, alongside
models and fonts), but **nothing currently consumes them**: they were used by the retired
in-engine editor. The Tauri/React editor ships its own icons via the
`lucide-react` package, and the host's in-viewport billboards are native solid-color
glyphs (no textures). The renderer still exposes `uploadSvgIcon` (`renderer_textures.cpp`)
if textured icons are wanted again, but there is no caller today.

Treat this directory as vestigial — leave it unless you are reviving textured icons or
cleaning up the asset copy.

## Adding a new icon (if reviving)

```sh
# From the repo root:
curl -fsSL https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/<name>.svg \
     -o engine/assets/icons/<name>.svg
```

Browse names at https://lucide.dev/icons/ — the URL slug is the filename without `.svg`.

## Current icons

| File | Lucide name | Originally used for |
|---|---|---|
| `box.svg` | box | Mesh asset fallback |
| `image.svg` | image | Texture asset fallback |
| `file.svg` | file | Unknown asset fallback |
| `lightbulb.svg` | lightbulb | PointLight billboard |
| `flashlight.svg` | flashlight | SpotLight billboard |
| `camera.svg` | camera | Camera billboard |
| `eye.svg` | eye | Entity visibility toggle (legacy) |
