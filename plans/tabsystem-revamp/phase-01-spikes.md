# Phase 01 — go/no-go spikes

**Status:** COMPLETED

## Goal

De-risk the two load-bearing unknowns *before* any dependent code exists: (a) can
`react-resizable-panels` v4 handle a layout tree whose **structure** changes at runtime,
and (b) do the WebKitGTK primitives the drag layer needs actually work in our webview.
Both spikes are throwaway code, deleted within this phase; the findings land as a short
"Spike results" section appended to this plan's `README.md`.

## What exists to build on

- `react-resizable-panels` `^4` (`editor/package.json`). The shadcn wrapper
  (`components/ui/resizable.tsx`) re-exports only `ResizablePanelGroup` (orientation prop),
  `ResizablePanel`, `ResizableHandle` (:45); `useDefaultLayout` and the `Layout` type are
  imported **directly from `react-resizable-panels`** (`app/Layout.tsx:23`), wired for
  persistence at `app/Layout.tsx:72-81`.
- `devMode` in the store (`state/store.ts`, toggled by the hidden five-click Scene-tab
  gesture `activateSceneTab`, `app/WindowTitlebar.tsx:55-67`) — the natural gate for scratch UI.
- The FLIP settle already proves WAAPI `node.animate` works in this webview
  (`WindowTitlebar.tsx:156-159` inside the settle `useLayoutEffect` :131-161), but not
  *during* an active pointer capture.

## Work

### 1. Spike A — rrp v4 dynamic structure

The shipped editor already has TWO dockspace islands with different root orientations: the
Scene `Layout` is a vertical-root group, while `AssetEditorWorkspace`
(`panels/AssetEditorWorkspace.tsx:448`) is a horizontal-root group whose `skeleton` / `clips`
panels and bottom timeline strip mount and unmount on `hasRig` / `hasClips`. That is the
concrete dynamic add/remove this spike reproduces. The headline capability the dock tree must
deliver is moving panel tabs between the VERTICAL side docks and the HORIZONTAL bottom dock
**within one main tab** (vertical↔horizontal is the whole point); the model is keyed per
dockspace kind (`scene` vs `assetEditor`), and all drag/portal/structure-hash machinery is
scoped to the single active island.

A dev-gated scratch component (e.g. `app/DockSpike.tsx`, mounted behind `devMode` from
`App.tsx`) that drives one `ResizablePanelGroup` through the operations the dock tree will
need:

- Add and remove children at runtime, reproducing the asset-editor's `hasRig` / `hasClips`
  panel mount/unmount (the `normalize`-driven leaf collapse and the drop-created leaf). Verify
  remaining panels reclaim space sanely and `defaultSize` / imperative sizing behaves on the
  *changed* set.
- Reconcile sizes after a structural change: drive sizes from controlled state (the future
  `DockBranch.sizes`), confirm `onLayoutChanged` round-trips, and exercise the v4 imperative
  handle — `GroupImperativeHandle.setLayout` via `useGroupRef`/`useGroupCallbackRef` — against
  the keyed-remount path, deciding which the dock tree adopts after add/remove. (v4 exposes
  `setLayout` and px `minSize`; the spike confirms their *behaviour* across structural
  add/remove, not their existence.)
- Pixel `minSize` fidelity: a px-min panel (the future viewport leaf, 520 px;
  `VIEWPORT_MIN_WIDTH`, `app/Layout.tsx:48`) inside nested groups at a 1280×720 window —
  px `minSize` is supported (`SizeUnit` includes `'px'`), so the cannot-collapse-while-attaching
  guarantee is expressible; the spike confirms it holds under structural change.
- Nested groups three deep (branch → branch → leaf), since orientation alternates per depth —
  matching the vertical-root Scene tree and the horizontal-root asset-editor tree.

**Go/no-go:** if structural add/remove cannot be made glitch-free even with a keyed-remount
fallback (key the group subtree by a structure hash, then rAF-force `emitLayoutSettled`),
record it and switch the plan to the dockview-as-engine plan B (see README) before phase 03.
The structure hash is a stable string of the subtree's ordered `(nodeId, node type,
orientation, children)` tuples — **sizes excluded** — used as the `ResizablePanelGroup`'s
React key: child add/remove/reorder remounts the group, resizes never do.

### 2. Spike B — WebKitGTK drag primitives

Same scratch surface, three checks:

- **Portal reparent:** the real first canvas candidate is the timeline canvas in
  `components/timeline/TimelineSurface.tsx` — render it (or a stateful stand-in: `useState`
  counter + an `<img>`) via `createPortal` into a host div, then `appendChild` the host div
  across two parents on a button click. Verify React state, the canvas/image (no reload flash),
  and a scroll position survive the reparent.
- **`document.elementFromPoint` during pointer capture:** `setPointerCapture` on one element,
  and on every `pointermove` hit-test under the cursor across other elements; confirm the
  hit-test resolves a drop target in a **differently-oriented sibling group** (the
  vertical↔horizontal cross-orientation drop the dock tree needs), while a
  `pointer-events: none` ghost follows the cursor (the ghost must never be the hit result).
- **WAAPI under/after capture:** run a `translateX` settle animation immediately after
  `releasePointerCapture` (the drop frame) — confirm no dropped first frame.

### 3. Record and delete

Append "Spike results" (a dated paragraph per spike: pass/fail, anything surprising, the
chosen reconciliation mechanism for rrp) to `plans/tabsystem-revamp/README.md`. Delete the
scratch component(s) in the same change.

## Verify

- `cd editor && bun run check` green; `make prepare-for-commit` clean.
- Manual via `make run`: both spike surfaces exercised in the real webview (devMode toggled
  via the five-click Scene-tab gesture), each check observed pass/fail before recording.
- The tree contains no spike code after the phase; the README carries the findings.
- A go/no-go line per spike exists in the README, and the phase-03+ plan is confirmed (or
  plan B is invoked, amending the README and the affected phase files before proceeding).
