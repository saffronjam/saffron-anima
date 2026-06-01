+++
title = 'Theme & fonts'
weight = 10
+++

# Theme & fonts

The editor ships a dark theme, two fonts (Roboto for the UI, Roboto Mono for numeric fields), and a default docking layout that lands the four panels in sensible places on first run. All three are set up once during `newUi`.

## Dark theme

`applyTheme` starts from ImGui's built-in dark style, then overrides the color slots from a small named palette — `IM_COL32` constants in a `theme` namespace: accent orange, a highlight blue, and a ladder of grays from `titlebar` to `background` to `propertyField`:

```cpp
constexpr ImU32 accent        = IM_COL32(236, 158,  36, 255);
constexpr ImU32 background    = IM_COL32( 36,  36,  36, 255);
constexpr ImU32 propertyField = IM_COL32( 15,  15,  15, 255);
```

Interactive accents (check marks, slider grabs, the docking preview) get the orange `accent`; hover and active separators get the blue `highlight`. The style also tightens the geometry — `FrameRounding = 2.5`, a 1px frame/window border, `TabRounding = 3.5`, a small `IndentSpacing`. `applyTheme` runs right after `CreateContext`, before the backends initialize.

## Two fonts, one monospace

Data fields read better in a monospace font, so the editor loads Roboto for the general UI and Roboto Mono for numeric/data fields. Both loads are optional and fall back to the built-in font if the file is missing:

```cpp
const std::string robotoPath = assetPath("fonts/Roboto-Regular.ttf");
if (std::filesystem::exists(robotoPath))
    io.FontDefault = io.Fonts->AddFontFromFileTTF(robotoPath.c_str(), 17.0f);

const std::string monoPath = assetPath("fonts/RobotoMono-Regular.ttf");
if (std::filesystem::exists(monoPath))
    ui.monoFont = io.Fonts->AddFontFromFileTTF(monoPath.c_str(), 16.0f);
```

Roboto becomes the default font; the mono face is stashed on the `Ui` and handed out by `uiMonoFont` for panels that want to push it around a numeric field. The `exists` guard means a stripped build with no font files still runs, just with ImGui's default face.

## Default dock layout

ImGui docking persists a layout to its `.ini` once you've moved panels around. A fresh install has no saved layout, so the editor seeds one with the DockBuilder API on the first frame, but only when the dockspace node is empty — a saved layout is left alone:

```cpp
ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
if (node == nullptr || node->IsLeafNode())  // empty (no saved layout) → seed a default
{
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->Size);
    ImGuiID center = dockId;
    const ImGuiID left   = DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    const ImGuiID bottom = DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    const ImGuiID leftBottom = DockBuilderSplitNode(left, ImGuiDir_Down, 0.55f, nullptr, nullptr);
    DockBuilderDockWindow("Hierarchy", left);
    DockBuilderDockWindow("Inspector", leftBottom);
    DockBuilderDockWindow("Assets",    bottom);
    DockBuilderDockWindow("Viewport",  center);
    DockBuilderFinish(dockId);
}
```

The splits give the familiar layout: Hierarchy top-left, Inspector below it, Assets along the bottom, and the [Viewport](../viewport-panel/) filling the center. Window names must match the `ImGui::Begin` titles in the panels for the dock assignment to take.

> [!TIP]
> Because the seed only fires when no layout is saved, once you rearrange panels your layout sticks across runs (ImGui writes it to its `.ini`). Delete that file to get the default back.

## In the code

| What | File | Symbols |
|---|---|---|
| Palette | `ui.cppm` | the `theme` namespace constants |
| Theme application | `ui.cppm` | `applyTheme` |
| Font loading | `ui.cppm` | `AddFontFromFileTTF`, `ui.monoFont`, `uiMonoFont` |
| Default layout | `ui.cppm` | `uiBeginFrame`, `DockBuilder*`, `layoutBuilt` |

## Related

- [ImGui integration](../imgui-integration/) — where `applyTheme` and the fonts are wired in
- [Viewport panel](../viewport-panel/) — the panel that fills the dock center
- [Inspector](../inspector/) — uses the mono font + `vec3Control` styling
