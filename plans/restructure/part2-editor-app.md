# Part 2 — `editor/source/main.cpp` → thin stub + new `Saffron.EditorApp` module

> Read `README.md` first. Do this part **last** — it adds a new module + DAG edges and
> depends on Editor/Control being stable.

## Context
`editor/source/main.cpp` (now 433 lines) is meant to be a thin entry point but carries the
whole editor-application assembly: the `Thumbnail`/`EditorState` structs (incl. the newer
billboard icons `pointLightIcon`/`spotLightIcon`/`cameraIcon`/`eyeIcon` and the viewer
`previewId`), the `thumbnailFor` cache closure, the `importToCatalog` extension router, the
`editor->on*` wiring, the editor `Layer` (`onUpdate`/`onUi`/`onRenderGraph` — control poll,
`renderScene` + gizmo + **billboards** + pick + panels + viewer), and the `onCreate`/`onExit`
bodies (incl. freeing **all** icon textures + the viewer texture before the renderer).
Goal: move all of it into a new engine module so `main.cpp` is a ~5-line stub.

## Why a NEW module (not extend `Saffron.Editor`)
The DAG already has **`Control → Editor`** (`control.cppm` imports `Saffron.Editor`). The
moved glue calls `se::pollControl` (Control) + `se::renderScene`/`addTonemapPass`/
`renderMeshThumbnail` (Rendering) + `se::uiRegisterTexture`/`uiMonoFont` (Ui) +
`se::importModel`/`saveProject` (Assets) + `se::run`/`attachLayer` (App). If that lived in
`Saffron.Editor`, Editor would have to `import Saffron.Control` → **`Editor → Control →
Editor` cycle**. So a new module **`Saffron.EditorApp`**, sitting *above* Control, is
required (not stylistic). Nothing imports it except the exe, so its new edges are all downhill.

## Target
New file `engine/source/saffron/editorapp/editor_app.cppm`:
```cpp
module;
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/glm.hpp>
#include <algorithm> ... <unordered_map>   // the set main.cpp currently uses
export module Saffron.EditorApp;
import Saffron.Core; App; Window; Rendering; Ui; Editor; Control; Scene; Assets;

namespace se   // non-exported: the app-internal glue
{
    struct Thumbnail { ... };          // moved from main.cpp
    struct EditorState { ... };        // moved from main.cpp (incl. the billboard icons + viewer)
    // the lifted closures as internal helpers: makeThumbnailFor, importToCatalog,
    // editorOnCreate(App&, shared_ptr<EditorState>), editorOnExit(App&, shared_ptr<EditorState>)
}
export namespace se
{
    /// Runs the Saffron editor: builds the AppConfig, attaches the editor layer
    /// (control poll, scene render, gizmo, billboards, panels), and calls se::run.
    auto runEditor(std::string title, u32 width, u32 height) -> int;
}
```
`runEditor` builds the `AppConfig`, sets `onCreate`/`onExit` to delegate to the internal
helpers over a `std::make_shared<EditorState>()`, and `return run(std::move(config));`.
The `EditorState`/`Thumbnail`/closures stay **non-exported** (only `runEditor` is public).

Final `main.cpp`:
```cpp
import Saffron.EditorApp;
int main() { return se::runEditor("Saffron Editor", 1600, 900); }
```
(`runEditor` takes plain `title/width/height` so `main.cpp` needs no other imports / no
`WindowConfig` type leak.)

## What moves vs stays
- **Moves (all of it — it only calls exported engine fns):** `Thumbnail`, `EditorState`,
  the icon loads (incl. `pointLight/spotLight/camera/eye` + `box/image/file`), the
  `thumbnailFor` closure (texture→`loadTextureAsset`, mesh→`renderMeshThumbnail`, else SVG
  icon), `registerBuiltinComponents` wiring, the bundled-cube seed, `importToCatalog`, the
  `editor->onImport/onCreateCube/onSaveProject/onLoadProject` + `onFileDropped` wiring, the
  `Layer` (onUpdate/onUi/onRenderGraph incl. `renderScene`+`drawGizmo`+`drawEditorBillboards`+
  `pickEntity`+panels+viewer), the Escape→`shouldClose` subscription, and the `onExit`
  teardown (unregister **every** ImGui texture incl. all icons + viewer `previewId`, clear
  caches, clear `assets.meshRefByUuid/textureRefByUuid`).
- **Stays in `main.cpp`:** `int main()` + the single `runEditor(...)` call.

## CMake
Add `source/saffron/editorapp/editor_app.cppm` to the `FILE_SET CXX_MODULES` in
`engine/CMakeLists.txt` **after `app.cppm`** (it imports App + everything). It's an
interface unit (the exe imports it), so it belongs in the file set, not PRIVATE sources.
`editor/CMakeLists.txt` is unchanged (the exe already links `Saffron::Engine`; `main.cpp`
stays its only source).

## Verify
Build `-j1` green. Bounded headless run, validation-clean, **clean exit** (no VMA
"unfreed dedicated allocations" — confirm every icon + the viewer texture is freed in the
moved `onExit`; this is exactly the class of bug that bit the billboard feature). Manual:
the editor opens with the same panels, drag-drop import works, thumbnails render, gizmo +
billboards + pick work, viewer opens, Save/Load project works, `se ping` succeeds.

## Risks
- **Teardown order** is load-bearing: `onExit` must `uiUnregisterTexture(...)` every
  thumbnail/icon/viewer texture **before** `run` reaches `destroyUi`/`destroyRenderer`
  (preserved automatically — `app.cppm` calls `config.onExit` before `destroyUi`). Make sure
  ALL icons are freed (the 3 light/camera icons were previously missed → leak).
- `thumbnailFor` is captured by both `registerBuiltinComponents` and the layer/asset panel —
  keep a single `std::function` and pass by `const&` (the panel/registry signatures take it
  that way); don't create two divergent closures.
- `EditorState`/`Thumbnail` stay in a **non-exported** `namespace se` block (don't widen the
  public surface). Only `runEditor` is exported.
- `main.cpp` includes `<imgui.h>` today; after the move it needs none of that — strip it to
  the 2-line stub.

## Critical files
`editor/source/main.cpp` (→ stub), new `engine/source/saffron/editorapp/editor_app.cppm`,
`engine/CMakeLists.txt` (+ the new module in the file set, after `app.cppm`), `app.cppm`
(reference only — `AppConfig`/`run`/`attachLayer` + the onExit-before-destroy ordering).
