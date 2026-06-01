# Phase 9: Theme, fonts, and dock-like layout parity

**Status:** NOT STARTED

<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

## Goal

Bring the TS/Tauri editor to visual + ergonomic parity with the retiring C++ ImGui
editor: replace the MVP's ad-hoc inline CSS with design tokens + a dark theme that
matches the engine's `theme::` palette, load Roboto + Roboto Mono (mono for
data/number fields, mirroring `uiMonoFont`), and adopt a resizable/dockable layout
that reproduces the default DockBuilder arrangement (Hierarchy/Inspector left, Assets
bottom, Viewport center). The viewport panel must keep its reparented native X11 child
window glued to its rect across every dock/panel split-resize, committing the final
bounds on resize-end so the per-tick `resize-native-viewport` volume stays bounded.

This is the last UI-polish phase before phase-10 retires the C++ editor. It adds **no
new control commands and no new shared DTOs** — it consumes the existing
`resize-native-viewport` command (phase-1), the `set-gizmo` command family (phase-2/4),
and the typed client + Zustand store from phase-3. By this phase all data panels
(phases 5–8) already exist; phase-9 only restyles and relayouts them.

**Depends on:** phase-8 (all panels — Hierarchy, Inspector, Assets, Environment,
RenderStats, MenuBar — exist and are mounted in a placeholder layout). Also relies on
phase-1's `resize-native-viewport` and phase-3's `ViewportPanel` bounds-sync glue +
`engineStatus` store + `client`.

## Current state (verified)

### MVP frontend (the starting point — worktree)
- The MVP is a single CSS-grid shell with **inline ad-hoc styling** in one global file:
  `wt:editor/src/styles.css` (259 lines). It hardcodes hex colors throughout (no
  tokens): `:root { color: #e5e7eb; background: #111316 }` (`styles.css:1-7`), buttons
  `#242930`/`#2d333b`/`#36404b` (`styles.css:20-46`), inputs `#0f1216` with
  `font-variant-numeric: tabular-nums` (`styles.css:48-58`). These hexes are **NOT** the
  engine palette (see below) — they must be re-derived from `theme::`.
- The shell is a fixed two-region grid, **not dockable/resizable**:
  `.app-shell { grid-template-rows: 44px 1fr }` (`styles.css:60-65`) and
  `.workspace { grid-template-columns: 260px 1fr }` (`styles.css:118-122`) — a fixed
  260px sidebar + viewport column, with `min-width: 900px; min-height: 620px` on the body
  (`styles.css:14-18`). There is no Assets-bottom region and no split handles.
- The MVP already references Roboto Mono **as a CSS family string only**, never loaded:
  `.viewport-placeholder span { font-family: "Roboto Mono", ui-monospace, … }`
  (`styles.css:247`) and `.app-shell` body font is `Inter, ui-sans-serif, …`
  (`styles.css:4-6`). No `@font-face`, no font files bundled in the frontend.
- `index.html` is bare: `<div id="root"></div>` + `<script src="/src/main.tsx">`
  (`wt:editor/index.html`), no font `<link>`/preload.
- `package.json` deps: React 19.2, Vite 7.2, `@vitejs/plugin-react` 5, TS 5.9,
  `lucide-react` 0.468, `@tauri-apps/api` 2.8 (`wt:editor/package.json`). **No
  layout/panel library** (no react-resizable-panels). lucide icons currently imported:
  `Box, Cable, CircleAlert, Move3D, Play, RefreshCw, Rotate3D, Scaling`
  (`wt:editor/src/main.tsx:4`).
- The viewport host is a single `<div ref={viewportRef} className="viewport-host">`
  (`wt:editor/src/main.tsx:306-308`, styled at `styles.css:214-220`,
  `background: #08090b`, `position: relative; overflow: hidden`). The engine reparents
  its X11 window over this div's rect.
- Bounds-sync already exists in the MVP and is the model to generalize:
  `viewportBounds()` reads `viewportRef.current.getBoundingClientRect()` and rounds
  `x/y/width/height` (`wt:editor/src/main.tsx:55-66`) — **note: no `scaleFactor`/DPR
  multiply yet** (HiDPI bug); a `useEffect` wires a `ResizeObserver` + a 250ms
  `setInterval` + `window.addEventListener("resize", …)` calling `resize_native_viewport`
  only on a real diff vs `lastViewportBoundsRef` (`wt:editor/src/main.tsx:156-190`).

### Engine theme + fonts + layout to match (main — the parity target)
- The C++ palette is `namespace theme` in `engine/source/saffron/ui/ui.cppm:30-48`
  (all `IM_COL32(r,g,b,a)`):
  - `accent = (236,158,36)` → `#EC9E24` (saffron orange — checkmarks, sliders, docking preview, nav)
  - `highlight = (39,185,242)` → `#27B9F2`
  - `background = (36,36,36)` → `#242424` (window/child bg)
  - `backgroundDark = (26,26,26)` → `#1A1A1A` (scrollbar bg, separators, header-active, docking-empty)
  - `titlebar = (21,21,21)` → `#151515` (menu/title/tab-dimmed bg)
  - `propertyField = (15,15,15)` → `#0F0F0F` (frame bg = input bg)
  - `text = (192,192,192)` → `#C0C0C0`
  - `textBrighter = (210,210,210)` → `#D2D2D2`
  - `textDarker = (128,128,128)` → `#808080` (disabled text)
  - `muted = (77,77,77)` → `#4D4D4D` (borders, buttons, frame-active)
  - `groupHeader = (47,47,47)` → `#2F2F2F` (headers, button-hover, frame-hover)
  - `selection = (237,192,119)` → `#EDC077`
  - `selectionMuted = (237,201,142,23)` → `rgba(237,201,142,0.09)` (text-selected bg)
  - `backgroundPopup = (63,70,77)` → `#3F464D` (popup bg)
- Style metrics (`ui.cppm:251-255`): `FrameRounding 2.5`, `FrameBorderSize 1.0`,
  `WindowBorderSize 1.0`, `TabRounding 3.5`, `IndentSpacing 11.0`. The axis-reset
  buttons use a 1px frame rounding (`ui.cppm:321`).
- Fonts (`ui.cppm:478-489`): UI default `Roboto-Regular.ttf` at **17px**, mono
  `RobotoMono-Regular.ttf` at **16px** loaded into `Ui.monoFont` (`ui.cppm:125`,
  `uiMonoFont` at `ui.cppm:144,633`). The inspector pushes the mono font for data fields
  (`editor_app.cppm:373: ImGui::PushFont(se::uiMonoFont(app.ui))`). The TTFs exist at
  `editor/assets/fonts/{Roboto-Regular.ttf,RobotoMono-Regular.ttf}` (both 33896/22780
  bytes; copied to `editor-old/assets/fonts/` after the phase-1 move — verified present
  in `wt:editor-old/assets/fonts/`).
- The default dock layout is built once in `uiBeginFrame` (`ui.cppm:558-579`):
  - `DockSpaceOverViewport` → split **Left 0.20** off center (`ui.cppm:569`)
  - split **Down 0.28** off the remaining center → bottom (`ui.cppm:570`)
  - split the left node **Down 0.55** → `leftBottom` (`ui.cppm:571`)
  - dock: `Hierarchy → left`, `Inspector → leftBottom`, `Environment → leftBottom`
    (tabbed with Inspector), `Assets → bottom`, `Viewport → center`
    (`ui.cppm:572-576`).
- Icon parity set (vendored Lucide SVGs, `editor/assets/icons/`): `box.svg`,
  `camera.svg`, `eye.svg`, `file.svg`, `flashlight.svg`, `image.svg`, `lightbulb.svg`.
  Their C++ usage mapping is in `editor_app.cppm:101-107` (mesh=box, texture=image,
  file=file, view=eye, point-light=lightbulb, spot-light=flashlight, camera=camera).
  These all have direct `lucide-react` equivalents: `Box`, `Camera`, `Eye`, `File`,
  `Flashlight`, `Image`, `Lightbulb` — so icon parity is name-mapping, not asset work.

### Phase-3/phase-1 plumbing this phase builds on
- `editor/src/panels/ViewportPanel.tsx` already owns the host div + generalized
  bounds-sync (ResizeObserver + window-resize + resize-end commit + `scaleFactor()`
  multiply), per the phase-3 `loadingOverlayDesign`/`viewportBridgeDecision`. Phase-9
  must ensure that sync **also re-fires on dock split-resize** (the ResizeObserver on the
  host div already covers panel resizes since the div geometry changes; the explicit
  resize-end commit must be wired to the panel library's drag-end too).
- `resize-native-viewport` (phase-1) does `XMoveResizeWindow + setViewportDesiredSize`
  only (no reparent/remap) — the per-tick command this phase fires repeatedly. The
  `LoadingOverlay` sibling layer (phase-3) sits over the viewport region until
  `engineStatus.phase === ready`; the theme must style it too.

### Non-goals (explicitly, so they are not read as gaps)
- **Persisted/serializable dock layouts** (ImGui writes `imgui.ini`; the C++ editor's
  layout is rebuilt each fresh start, never user-saved). Parity = the *default* layout +
  live resizing; do NOT add layout persistence.
- **Tab-drag re-docking / detaching panels into OS windows** — ImGui docking allows
  arbitrary re-docking; we match the *default* arrangement with resizable splits only.
  Free-form drag-to-redock is out of scope (timebox to parity).
- Multi-viewport / multiple Tauri windows (whole-migration non-goal).

## Implementation

Ordered steps. All paths under `editor/` unless noted. Build/check inside the toolbox
(`bun run check`); the engine is unaffected by this phase (no C++ changes).

### 1. Design tokens (`editor/src/styles/tokens.css`)
Create `editor/src/styles/tokens.css` exposing the engine palette as CSS custom
properties on `:root`, derived 1:1 from `theme::` (`ui.cppm:32-47`). Use these exact
hex values so the TS editor reads as the same product:
```css
:root {
  /* surfaces */
  --bg:            #242424; /* theme::background      */
  --bg-dark:       #1a1a1a; /* theme::backgroundDark  */
  --titlebar:      #151515; /* theme::titlebar        */
  --field:         #0f0f0f; /* theme::propertyField   */
  --popup:         #3f464d; /* theme::backgroundPopup */
  --group-header:  #2f2f2f; /* theme::groupHeader     */
  --muted:         #4d4d4d; /* theme::muted (borders) */
  /* text */
  --text:          #c0c0c0; /* theme::text            */
  --text-bright:   #d2d2d2; /* theme::textBrighter    */
  --text-dim:      #808080; /* theme::textDarker      */
  /* accent / state */
  --accent:        #ec9e24; /* theme::accent          */
  --highlight:     #27b9f2; /* theme::highlight       */
  --selection:     #edc077; /* theme::selection       */
  --selection-bg:  rgba(237,201,142,0.09); /* theme::selectionMuted */
  /* axis colors (match the C++ vec3Control red/green/blue reset buttons) */
  --axis-x:        #b91c1c;
  --axis-y:        #15803d;
  --axis-z:        #1d4ed8;
  /* metrics (mirror ui.cppm:251-255) */
  --radius:        2.5px;   /* FrameRounding   */
  --radius-tab:    3.5px;   /* TabRounding     */
  --border-w:      1px;     /* Frame/WindowBorderSize */
  --font-ui:       "Roboto", ui-sans-serif, system-ui, sans-serif;
  --font-mono:     "Roboto Mono", ui-monospace, SFMono-Regular, Menlo, monospace;
  --font-size-ui:  14px;
  --font-size-data:13px;
}
```
> Keep the C++ point sizes (17/16) as a ratio, not literal px — webview CSS px at
> `--font-size-ui: 14px` reads equivalently at typical scale; the ratio (data ≈ 1px
> smaller, mono) is what matters for parity. The MVP axis colors at `styles.css:190-200`
> already match these red/green/blue values; promote them to `--axis-*`.

### 2. Dark theme (`editor/src/styles/theme.css`)
Create `editor/src/styles/theme.css` that consumes only tokens (no raw hex). Restyle the
primitive elements the MVP styled inline:
- `body`/`#root`: `background: var(--bg); color: var(--text); font: var(--font-size-ui)/1.4 var(--font-ui);`
  Drop the MVP's `min-width: 900px` (`styles.css:14-18`) — the dockable layout handles
  small sizes via panel min-sizes.
- `button`: bg `var(--muted)`, hover `var(--group-header)`, active `var(--bg-dark)`,
  border `var(--border-w) solid var(--muted)`, radius `var(--radius)` — mirrors the
  ImGui Button/Hovered/Active mapping (`ui.cppm:217-219`). `.active` (toggled, e.g.
  gizmo mode) → border `var(--accent)`.
- `input`, `select`, `textarea`: bg `var(--field)`, hover/focus border `var(--group-header)`/`var(--accent)`
  (mirrors FrameBg/Hovered/Active `ui.cppm:209-211`), `font-family: var(--font-mono);
  font-variant-numeric: tabular-nums;` (data fields are mono — see step 4).
- Panel chrome: panel title bars `var(--titlebar)` (`ui.cppm:205`), panel body `var(--bg)`,
  section/group headers `var(--group-header)` with uppercase 12px labels (port the
  MVP's `.sidebar h2` uppercase style at `styles.css:130-136`), separators
  `var(--bg-dark)` (`ui.cppm:241`).
- Selection row: `.selected { background: var(--selection-bg); color: var(--text-bright); }`
  (mirrors TextSelectedBg + the MVP `.entity-row.selected` at `styles.css:146-149`,
  retinted to tokens).
- Scrollbars (webkit): track `var(--bg-dark)`, thumb `var(--muted)`, hover
  `var(--group-header)`, active `var(--accent)` (`ui.cppm:227-230`).
- Checkbox accent: `accent-color: var(--accent);` (CheckMark/SliderGrab `ui.cppm:237-238`).
- `LoadingOverlay` (phase-3): bg `var(--bg)` with the spinner tinted `var(--accent)`,
  error text `var(--highlight)`/red, Retry/Restart buttons styled as above. Style it here
  so the overlay matches the shell.

`editor/src/styles.css` becomes a thin entry that `@import`s `tokens.css` then
`theme.css` then the per-component sheets, replacing the MVP's monolithic file. Delete
the dead MVP class rules (`.app-shell`, `.workspace`, `.viewport-column` fixed grids)
that the dockable layout (step 5) supersedes; keep the still-used ones (`.axis-field`,
`.tool-group`, `.viewport-host`/`.viewport-frame`) retinted to tokens.

### 3. Bundle Roboto + Roboto Mono
The C++ TTFs live at `editor-old/assets/fonts/` (after phase-1) and are copied to the
runtime dir for the engine — the **frontend cannot read the runtime dir**, so bundle the
fonts with the webview app:
- Copy `Roboto-Regular.ttf` + `RobotoMono-Regular.ttf` into
  `editor/src/assets/fonts/` (Vite will fingerprint+inline them). Optionally also add
  `Roboto-Medium`/`Bold` if the C++ UI uses weight for group headers — the C++ side only
  ships Regular (`ui.cppm:480-489`), so Regular is sufficient for parity; faux-bold via
  `font-weight` is acceptable for the few bold labels (`styles.css:86,134`).
- In `tokens.css` (or a dedicated `editor/src/styles/fonts.css` imported first), declare:
  ```css
  @font-face { font-family: "Roboto"; src: url("../assets/fonts/Roboto-Regular.ttf") format("truetype"); font-weight: 400; font-display: swap; }
  @font-face { font-family: "Roboto Mono"; src: url("../assets/fonts/RobotoMono-Regular.ttf") format("truetype"); font-weight: 400; font-display: swap; }
  ```
- `editor/index.html`: add `<link rel="preload" as="font" type="font/ttf" href="/src/assets/fonts/Roboto-Regular.ttf" crossorigin>` (and the mono one) inside `<head>` so the first paint isn't FOUT. Update `<title>` to `Saffron Editor` (already correct in MVP).
> Do NOT use a Google-Fonts CDN `<link>` — Tauri webviews run offline/CSP-restricted;
> fonts must be bundled. The MVP's `Inter` default (`styles.css:4-6`) is dropped.

### 4. Mono for data/number fields (parity with `uiMonoFont`)
The C++ inspector pushes the mono font only around data fields (`editor_app.cppm:373`).
Match that selectivity, not a global mono:
- The shared numeric/vector primitives from phases 3/6 (`editor/src/components/VectorEditor.tsx`,
  `NumberDrag.tsx`, `editor/src/components/fieldRenderer.tsx`) render their `<input>`s and
  value readouts in `var(--font-mono)` (set via the `input` rule in step 2 + an explicit
  `.data-field`/`.mono` class for non-input value displays like RenderStats numbers).
- UI chrome (labels, buttons, menu items, hierarchy names) stays `var(--font-ui)`
  (Roboto). RenderStatsPanel numbers, entity ids, asset ids, transform/material values →
  mono. This mirrors the C++ "data fields monospace" rule exactly.

### 5. Resizable/dockable layout (`editor/src/app/Layout.tsx`)
Add `react-resizable-panels` to `editor/package.json` deps (mature, headless,
keyboard-accessible, ~tiny). Create `editor/src/app/Layout.tsx` that reproduces the
default DockBuilder arrangement (`ui.cppm:568-577`) with nested `PanelGroup`s:
```
<PanelGroup direction="vertical">              // outer: top region + Assets bottom
  <Panel defaultSize={72} minSize={30}>        // top region
    <PanelGroup direction="horizontal">        // Left 0.20 | Center
      <Panel defaultSize={20} minSize={12}>     // left column (ui.cppm:569 = 0.20)
        <PanelGroup direction="vertical">       // Hierarchy / (Inspector+Environment tabs)
          <Panel defaultSize={45}><Hierarchy/></Panel>   // 1 - 0.55 (ui.cppm:571)
          <PanelResizeHandle/>
          <Panel defaultSize={55}>             // leftBottom (0.55)
            <TabbedPanel tabs={[Inspector, Environment]}/>  // both dock to leftBottom (ui.cppm:573-574)
          </Panel>
        </PanelGroup>
      </Panel>
      <PanelResizeHandle/>
      <Panel minSize={30}><ViewportPanel/></Panel>   // center (ui.cppm:576)
    </PanelGroup>
  </Panel>
  <PanelResizeHandle/>
  <Panel defaultSize={28} minSize={12}><AssetsPanel/></Panel>  // bottom (ui.cppm:570 = 0.28)
</PanelGroup>
```
- `defaultSize` percentages match the DockBuilder splits: left 20, bottom 28, leftBottom
  55 (so Hierarchy is the top 45 of the left column).
- Implement a small `TabbedPanel` (Inspector | Environment tabs) — ImGui tabs the two
  into the same node (`ui.cppm:573-574`); reproduce with a simple tab strip styled with
  `--radius-tab`/`--titlebar`/`--accent` (Tab/TabSelected mapping `ui.cppm:221-225`).
  RenderStats from phase-8 can be a third tab here or a strip in the Topbar/statusbar —
  the C++ editor floats Render Stats as a window; tabbing it next to Inspector is
  acceptable parity. Pick one and note it.
- `PanelResizeHandle` styled as a 4px gutter using `--bg-dark`/`--muted` hover, matching
  the ImGui separator colors (`ui.cppm:241-243`).
- The MenuBar (phase-8) + Topbar gizmo group (phase-4) render **above** the `Layout`
  PanelGroup; the statusbar (`styles.css:251-258`, retinted) renders below. `App.tsx`
  composes: `<MenuBar/> <Topbar/> <Layout/> <StatusBar/>` in a column flex; the
  `LoadingOverlay` is a sibling absolutely-positioned layer over the viewport region
  (phase-3 sibling-stacking requirement — it must NOT be a child the X11 window paints
  over).

### 6. Viewport bounds re-emit on split-resize (`editor/src/panels/ViewportPanel.tsx`)
The phase-3 ViewportPanel already syncs bounds via a `ResizeObserver` on the host div.
Dock splits change the host div geometry, so the observer fires — but rapid drag
multiplies `resize-native-viewport` calls (risk: flicker/queue backlog). Harden:
- Keep a throttled live sync (the existing ResizeObserver) firing `resize-native-viewport`
  at most every ~50ms during a drag so the native window roughly tracks.
- Add an **explicit resize-end commit**: subscribe to the panel library's drag-end via
  `PanelGroup`'s `onLayout` callback debounced (~150ms after the last change) AND a
  `pointerup`/`window`-resize-end, committing the final exact bounds once. Mirror the
  MVP's diff guard (`wt:editor/src/main.tsx:166-179`) so a no-op layout never sends.
- Multiply CSS-px bounds by `await getCurrentWindow().scaleFactor()` (or cache the DPR
  from a `scaleFactor` listener) before sending — fixing the MVP HiDPI gap
  (`wt:editor/src/main.tsx:55-66` rounds raw CSS px). Phase-3 introduced this; verify it
  is applied to the split-resize path too.
- Gate sync OFF while `engineStatus.phase !== ready` (no native window mapped yet) and
  while the LoadingOverlay is shown.

### 7. Icon parity (`lucide-react` name map)
Replace ad-hoc MVP icons with the lucide equivalents of the C++ SVG set
(`editor_app.cppm:101-107`): mesh→`Box`, texture→`Image`, file→`File`, view→`Eye`,
point-light→`Lightbulb`, spot-light→`Flashlight`, camera→`Camera`. Add gizmo-mode icons
already in the MVP (`Move3D`/`Rotate3D`/`Scaling`, `wt:main.tsx:4`) to the Topbar group.
These are used in AssetsPanel tiles (phase-7), the CreateMenu (phase-5), and the Topbar
(phase-4); phase-9 just standardizes the mapping + sizes (16px chrome, tinted
`var(--text)`/`var(--accent)` when active).

### 8. Keyboard shortcuts (W/E/R) — spike-0b gated
Add W/E/R (translate/rotate/scale) + a world/local toggle key, calling `client.setGizmo`
(phase-2/4) when the webview has focus. **Only** bind these in the webview when the
spike-0b input model (phase-3) is **control-command-driven** (the default assumption); if
spike-0b chose raw-keyboard-into-the-child-window, the engine already handles W/E/R
(C++ `editor_gizmo.cpp` W/E/R cycle) and the webview must NOT also bind them (double
input). Read the recorded spike-0b decision before wiring; guard with a comment
referencing it. Shortcuts must not fire while a text input is focused.

### 9. CMake / build
No engine CMake change. Frontend only:
- `editor/package.json`: add `react-resizable-panels` dependency; the font TTFs are
  imported as assets by Vite (no script change).
- `editor/vite.config.ts`: ensure `assetsInclude` covers `**/*.ttf` (Vite handles `.ttf`
  by default; confirm). Run `bun run check` (tsc `--noEmit`) — purely CSS/TSX, no type
  surface change.

## Done when

- [ ] `editor/src/styles/tokens.css` + `theme.css` exist; **no raw hex** remains in
      component styles (only `var(--…)`); the palette values match `theme::`
      (`ui.cppm:32-47`) exactly (accent `#ec9e24`, bg `#242424`, field `#0f0f0f`, etc.).
- [ ] The editor visually matches the C++ dark theme: saffron-orange accent on
      checkmarks/sliders/active tabs, `#242424` panels, `#151515` title bars,
      `#0f0f0f` input fields, `#c0c0c0` text — side-by-side with `editor-old`
      SaffronEditor it reads as the same product.
- [ ] Roboto + Roboto Mono are bundled (`editor/src/assets/fonts/`) and loaded via
      `@font-face` + `index.html` preload (no CDN); first paint shows Roboto, not the
      MVP's Inter/system fallback.
- [ ] **Data/number fields are monospace** (VectorEditor/NumberDrag inputs, RenderStats
      numbers, entity/asset ids) while chrome (labels/buttons/menus/hierarchy names) is
      Roboto — matching the C++ `uiMonoFont` usage (`editor_app.cppm:373`).
- [ ] Panels are resizable; the **default layout is Hierarchy + (Inspector/Environment
      tabbed) on the left, Assets on the bottom, Viewport center**, with split ratios
      ~left 0.20 / bottom 0.28 / leftBottom 0.55 (matching `ui.cppm:568-577`).
- [ ] Dragging a panel split resizes neighbors smoothly; the reparented native viewport
      window stays glued to its panel rect with **no persistent flicker** — a final
      `resize-native-viewport` commits the exact bounds on drag-end (diff-guarded), and
      HiDPI bounds are `scaleFactor`-corrected.
- [ ] During fast split-drags the native window may lag ≤1 frame (accepted) but never
      detaches, double-presents, or floods the socket (throttled live sync + commit).
- [ ] lucide icons match the C++ SVG set mapping (Box/Image/File/Eye/Lightbulb/
      Flashlight/Camera) in Assets/Create/Topbar.
- [ ] W/E/R gizmo shortcuts work per the recorded spike-0b model (command-driven by
      default; NOT bound in the webview if input is native), and do not fire while a text
      field is focused.
- [ ] The `LoadingOverlay` (phase-3) is themed (token bg + accent spinner) and still
      occupies a sibling layer over the viewport region (not painted over by the X11
      window).
- [ ] `bun run check` passes; no console errors/warnings during panel resize or font
      load; `editor-old/` SaffronEditor still builds + runs (this phase touches no C++).

## Risks / seams

- **Resize volume / flicker.** Dock splits multiply `resize-native-viewport`. Mitigate
  with a throttled live sync (~50ms) + a single diff-guarded resize-end commit
  (debounced `onLayout` + pointerup), mirroring the MVP diff guard
  (`wt:main.tsx:166-179`). The native window can tear/lag ≤1 frame during fast drags —
  accepted (same 1-frame-lag tradeoff as the C++ viewport resize).
- **Overlay/popover stacking over the X11 window.** The native viewport window always
  paints on top of its rect. Any menu/tab popover that would overlap the viewport must
  render outside the viewport rect or while the window is unmapped (phase-3
  `loadingOverlayDesign`); the tabbed Inspector/Environment + MenuBar live in non-viewport
  regions so they are safe, but the gizmo-mode popover / dropdowns must respect this.
- **Font sizing.** C++ uses 17px/16px ImGui point sizes; webview CSS px at 14/13 reads
  equivalently but is a judgment call — tune against a side-by-side screenshot; do not
  chase pixel-exactness (timebox).
- **Theme polish is open-ended.** Scope is *parity with the existing dark theme*, not a
  redesign — stop when it matches `theme::`. New colors require a token, never inline hex.
- **Layout persistence is a non-goal** (the C++ editor rebuilds its layout each start);
  do not add `imgui.ini`-style saved layouts here — it would creep scope and is not
  parity.
- **Spike-0b dependency.** The W/E/R shortcut binding is conditional on the phase-3 input
  decision; wiring it before reading that decision risks double keyboard input. Seam: a
  single `INPUT_MODEL` constant/flag from phase-3 gates the webview key bindings.
- **No new control commands / DTOs** — this phase is pure frontend; the contract test and
  protocol generation are untouched.
