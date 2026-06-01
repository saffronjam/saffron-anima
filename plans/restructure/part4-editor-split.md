# Part 4 — split `editor/editor.cppm` (911 lines)

> Read `README.md` in this folder first for the validated mechanism + build/gate rules.

## Context
`engine/source/saffron/editor/editor.cppm` is `module Saffron.Editor`, imports
`{Core, Signal, Scene, Json, Ui}` (NOT Rendering — it never touches `Renderer`). It has
grown to 911 lines: the editor camera, the `EditorContext`, a 180-line component-
registration function, the panels, the gizmo, and the newer **billboards + viewer**.
Goal: split into one interface partition (shared types + decls) + `.cpp` impl units, same
pattern as the renderer. Pure reorg, public surface unchanged.

## Current exported surface (decls go to `:Context`, defs to impl units)
Types: `EditorCamera`, `EditorContext`, `AssetDragPayload`.
Functions: `setSelection`, `drawAssetPicker`, `registerBuiltinComponents`,
`newEditorContext`, `destroyEditorContext`, `hierarchyPanel`, `inspectorPanel`,
`drawImportModal`, `assetCatalogPanel`, `viewerPanel`, `drawEditorMenuBar`,
`editorCameraForward`, `editorCameraView`, `updateEditorCamera`, `drawGizmo`,
`drawEditorBillboards`.

## Target files (under `engine/source/saffron/editor/`)
| File | kind | contents |
|---|---|---|
| `editor_context.cppm` | **`:Context` interface partition** | `EditorCamera`, `EditorContext`, `AssetDragPayload` + **all** the public function declarations above. GMF: imgui, ImGuizmo (the `EditorContext::gizmoOp` field initializer needs it), entt, glm. `import Saffron.Core/Signal/Scene/Json/Ui`. |
| `editor_context.cpp` | impl unit | `setSelection`, `newEditorContext`, `destroyEditorContext` |
| `editor_components.cpp` | impl unit | `drawAssetPicker`, `registerBuiltinComponents` (the 180-line monolith — stays one function; keep the 8-component order) |
| `editor_panels.cpp` | impl unit | `hierarchyPanel`, `inspectorPanel`, `drawImportModal`, `assetCatalogPanel`, `viewerPanel`, `drawEditorMenuBar` |
| `editor_camera.cpp` | impl unit | `editorCameraForward`, `editorCameraView`, `updateEditorCamera` |
| `editor_gizmo.cpp` | impl unit | `drawGizmo`, `drawEditorBillboards` |
| `editor.cppm` (primary, edit in place) | primary interface | GMF + `export module Saffron.Editor;` + `export import :Context;` + the module `import`s. No definitions left. |

Each impl unit: `module Saffron.Editor;` + its own GMF (imgui / imgui_stdlib for
`InputText(&std::string)` / ImGuizmo / entt / glm + gtx/gtc as used) + `import Saffron.Core;`
+ whatever sibling modules it calls (`Scene`, `Ui`, `Json`, `Signal`). `EditorContext` and
the other types come via the implicit primary-interface import (which `export import`s
`:Context`). Cross-unit calls (e.g. `registerBuiltinComponents` → `drawAssetPicker`, both
in `editor_components.cpp` → co-located; panel→`setSelection` → declared in `:Context`) resolve via the `:Context` decls.

## Steps (build `-j1` + gate after each)
1. Create `editor_context.cppm` (`:Context`) with the 3 types + all public fn decls. Edit
   `editor.cppm`: keep only GMF + `export module Saffron.Editor;` + `export import :Context;`
   + the module imports; move the type defs into `:Context`. Add `:Context` to the
   `FILE_SET CXX_MODULES` in `engine/CMakeLists.txt` **before** `editor.cppm`. Build `-j1`.
   (At this point the primary still holds all the function definitions — that compiles.)
2. Extract impl units one at a time (`sed -n` the function ranges from `editor.cppm` into a
   new `module Saffron.Editor;` `.cpp`, including each function's leading comment; delete
   the ranges from `editor.cppm`; add the `.cpp` to `target_sources(SaffronEngine PRIVATE …)`).
   Order: components → panels → camera → gizmo → context. Build `-j1` after each.
3. `editor.cppm` should end as just the module decl + `export import :Context;` + imports
   (no definitions), or keep a couple of trivial ones if cleaner.

## Verify
Build `-j1` green. Bounded headless run (`SAFFRON_EXIT_AFTER_FRAMES=5`) is validation-clean
and exits cleanly. Drive the running editor: `se list-entities`, `se screenshot viewport`
(reads OK). Manual UI smoke: hierarchy select, inspector add/remove component, gizmo W/E/R,
**billboards draw + click-select**, asset panel + drag-drop, viewer panel, menu Create/Save/Load.

## Risks
- `:Context` must `import Saffron.Signal` (`EditorContext::onSelectionChanged` is a
  `SubscriberList<Entity>`) and `#include <ImGuizmo.h>` (the `gizmoOp` field initializer).
- `registerBuiltinComponents` is the biggest unit (keep it one function; its inline lambdas
  call `drawAssetPicker` → keep both in `editor_components.cpp`).
- `editor_panels.cpp`'s `assetCatalogPanel` is the one with the drag-drop `BeginDragDropSource`
  (already uses `ImGuiDragDropFlags_SourceAllowNullID` — don't regress that).
- Don't add "moved from editor.cppm" comments to the relocated functions.

## Critical files
`engine/source/saffron/editor/editor.cppm` (+ new `editor_context.cppm` + 5 `.cpp`),
`engine/CMakeLists.txt`. Reference the renderer for the exact pattern:
`engine/source/saffron/rendering/renderer_types.cppm` (interface partition) and
`renderer_drawlist.cpp` (impl unit).
