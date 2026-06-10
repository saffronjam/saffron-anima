# Phase 11 — editor bottom dock + tools-style tab system

**Status:** COMPLETED

## Goal

Add the **bottom dock** the timeline will live in: a new full-width resizable region below the viewport,
with a tab system copied from the right "tools" sidebar (a `BottomTool` union, open/close, vanishes when
empty), opened from a `Topbar` button, with its height persisted and its open/close/resize firing the
layout bus so the Wayland subsurface viewport re-glues. This phase ships the **empty dock** — it opens,
closes, resizes, and the viewport tracks it correctly. Phase 12 fills it with the timeline.

## What exists to build on

- `Layout.tsx:176-191` — the right region is a **vertical** `ResizablePanelGroup`: `viewport`
  (`defaultSize=72`) + `ResizableHandle` + `assets` (`defaultSize=28`). The outer container is a flex with
  `aside` (left sidebar) + left vertical group + main + right vertical group.
- The right "tools" model (the template to copy):
  - `RightTool = 'stats' | 'profiler'` (`store.ts:44`); `rightTools: RightTool[]` + `activeRightTool`
    (`store.ts:87-88`, init `[]`/`null`).
  - `openRightTool`/`closeRightTool` (`store.ts:483-502`) — add/remove + reassign active, null when empty.
  - `RightSidebar.tsx` — `TOOL_LABEL` map (`:11`), a `role="tablist"` `h-8` strip with `role="tab"`
    items + an X close button per tab (`:23-64`).
  - Layout removes the sidebar when empty: `{rightToolsOpen ? (...) : null}` (`Layout.tsx:195-208`),
    viewport reclaims the width.
  - Width persistence: `loadRightSidebarWidth`/`persistRightSidebarWidth` keyed by project path, prefix
    `saffron.layout.rightSidebarWidth:`, default 320 (`store.ts:814-843`, `Layout.tsx:48,58-59`).
- Layout bus / bounds-sync: `onLeftLayoutChanged`/`onRightLayoutChanged` call `emitLayoutSettled()`
  (`Layout.tsx:72-79`); `LeftBottomTabs.onTabChange` fires `emitLayoutSettled({force:true})` via rAF
  (`Layout.tsx:224-232`); `ViewportPanel` listens with `onLayoutSettled` (`:239`) and on every geometry
  change re-commits the subsurface bounds (`:230-239`). **A new dock just needs to emit on the same bus.**
- `Topbar.tsx` — the playback button group (`role="group"`, `Button size="icon-sm"`, `:111-168`) is the
  styling reference; tools are opened from here.

## Work

### 1. Store: `BottomTool` slices (mirror the right-tools slices exactly)

In `store.ts`:
- `export type BottomTool = 'timeline'` (extensible union; `'timeline'` is the only member until more
  bottom tools exist).
- `bottomTools: BottomTool[]` (init `[]`) + `activeBottomTool: BottomTool | null` (init `null`).
- `openBottomTool(tool)` / `closeBottomTool(tool)` / `setActiveBottomTool(tool)` — copy the bodies of
  `openRightTool`/`closeRightTool`/`setActiveRightTool` verbatim.
- `loadBottomDockHeight(path)` / `persistBottomDockHeight(path, h)` — copy the right-sidebar-width pair,
  prefix `saffron.layout.bottomDockHeight:`, sensible default (e.g. `220`).

### 2. Layout: insert the full-width bottom dock

Reshape `Layout.tsx` so the dock spans the full editor width below the viewport/assets region (the
reference-engine + mock placement). Two acceptable approaches (see README options):
- **Full-width (recommended):** wrap the left sidebar + center column in an outer container and add the
  bottom dock as a region spanning the center/right width below the viewport+assets group. This is the
  larger diff and needs a one-time layout-persistence reset (no layout versioning exists today — acceptable
  per the README; reset `saffron.layout.*` on first run after the change).
- **Smaller diff:** add a 3rd `ResizablePanel` (`id="bottom-dock"`, `defaultSize≈22`, `minSize≈8`) +
  `ResizableHandle` inside the existing right vertical group, stacking viewport / assets / dock. Reuses
  react-resizable-panels' own persistence; spans only center+right width.

Conditionally render the dock only when `bottomTools.length > 0` (mirror `rightToolsOpen`), so closing all
bottom tools removes it and the viewport reclaims the height.

### 3. Bounds-sync + the tab strip

- On dock open/close and on resize-end, call `emitLayoutSettled({force:true})` (the `LeftBottomTabs`
  precedent) so `ViewportPanel`'s existing listener re-commits the subsurface bounds. **No `ViewportPanel`
  change is needed** — it already listens.
- Create `BottomDock.tsx` parallel to `RightSidebar.tsx`: the `role="tablist"` `h-8` strip with a
  `BOTTOM_TOOL_LABEL` map, `role="tab"` items dispatching `setActiveBottomTool`, and an X per tab calling
  `closeBottomTool`. The active tool's panel renders below the strip (Phase 12 supplies the timeline
  component; here it can be a placeholder).

### 4. Topbar trigger

Add a `Topbar` button (a `Film`/`Clapperboard` lucide icon, styled like the existing icon buttons) that
calls `openBottomTool('timeline')`.

## Validation (done criteria)

- `bun run check` (protocol regen + typecheck) + `bun run lint` clean.
- Manual (`make run`): clicking the Topbar button opens an empty bottom dock; it resizes via the handle;
  the **viewport subsurface stays correctly aligned** while resizing and after open/close (the bounds-sync
  works — this is the one real risk); closing the last tab removes the dock and the viewport reclaims the
  space.
- `docs/`: note the bottom dock in the editor layout docs (or the timeline page once Phase 12 lands).

## Notes / gotchas

- **The viewport is a live Wayland subsurface composited under the transparent webview** — any layout
  change that moves the viewport rect must go through `emitLayoutSettled`, or the engine frame and the UI
  will visibly desync. This is the single thing to get right in this phase; the dock chrome is trivial by
  comparison.
- **Layout persistence has no versioning today** — adding a region changes the persisted split shape, so
  old persisted `saffron.layout.*` may not include the dock. A one-time reset on first run is acceptable
  (README). If the smaller-diff (3rd panel) approach is taken, react-resizable-panels handles persistence
  and no reset is needed.
- Keep the dock a sibling of the existing tools model conceptually — do not entangle `bottomTools` with
  `rightTools`; they're parallel, independent slices.
