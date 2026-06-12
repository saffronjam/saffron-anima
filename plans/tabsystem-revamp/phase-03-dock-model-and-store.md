# Phase 03 — dock model, store unification, centralized settle

**Status:** NOT STARTED

## Goal

Introduce the pure dock-layout tree (`state/dockLayout.ts`) with unit tests, replace the
three parallel tab/tool store slices with one `dockLayout` slice, and centralize the
viewport re-glue: a single store subscriber fires `emitLayoutSettled` on every dock
mutation, retiring the per-site emitters in the same change. **Zero visual change** — the
three regions render exactly as before, now reading from the tree's three well-known leaves.

## What exists to build on

- Slices to be replaced (`state/store.ts`): `BottomTab` (`:43`) + `bottomTab` (`:85`) +
  `setBottomTab` (`:394`); `RightTool` (`:46`) + `rightTools`/`activeRightTool` (`:95-96`);
  `BottomTool` (`:50`) + `bottomTools`/`activeBottomTool` (`:99-100`); the duplicated action
  trios `openRightTool`/`closeRightTool`/`setActiveRightTool` and the bottom equivalents
  (`:532-571`). Close-fallback contract: active falls to the index−1 neighbor (`:546`).
- Read/write sites to migrate:
  - `panels/Topbar.tsx:291` (`openBottomTool("timeline")`), `:306-310` (wrench menu →
    `openRightTool`).
  - `components/AlarmBadge.tsx:31` (`openRightTool(target)`).
  - `panels/HierarchyTree.tsx:371` (`setBottomTab("inspector")` deep-link).
  - `app/Layout.tsx`: `rightToolsOpen`/`bottomToolsOpen` selectors (`:55-56`), the
    `bottomToolsOpen` settle effect (`:88-91`), `LeftBottomTabs`'s `setBottomTab` +
    rAF-settle handler (`:257-262`).
  - `panels/RightSidebar.tsx` / `panels/BottomDock.tsx` (their whole state surface).
  - The metrics-poll gate `rightTools.includes("stats")` (`store.ts:1294`).
- `resetSceneState` (`store.ts:645`) resets `viewTabs` but leaves tool state alone — keep
  that: it must not touch `dockLayout`.
- The settle bus: `app/layoutBus.ts` (`emitLayoutSettled`), consumed by `ViewportPanel`.
- `tools/ci/check.sh` runs the frontend gate (`bun run build`); no editor unit tests exist
  anywhere yet.

## Work

### 1. `state/dockLayout.ts` — pure, DOM-free

```ts
export type DockPanelId =
  | "inspector" | "environment" | "render"   // today: BottomTab
  | "stats" | "profiler" | "material"        // today: RightTool
  | "timeline"                               // today: BottomTool
  | "hierarchy" | "assets" | "viewport";     // join the tree in phase 07

export type DockNodeId = string;

export interface DockLeaf {
  type: "leaf"; id: DockNodeId;
  tabs: DockPanelId[];                       // order = strip order
  activeTab: DockPanelId | null;
  locked?: boolean;                          // viewport: no drops, tab not draggable
  persistent?: boolean;                      // normalize never deletes it, even empty
}
export interface DockBranch {
  type: "branch"; id: DockNodeId;
  orientation: "horizontal" | "vertical";    // alternates per depth (VS Code gridview)
  children: DockNodeId[];
  sizes: Record<DockNodeId, number>;         // percents, rrp v4 layout shape
}
export type DockNode = DockLeaf | DockBranch;
export interface DockLayout { version: 1; rootId: DockNodeId; nodes: Record<DockNodeId, DockNode> }

export type DropTarget =
  | { kind: "tab"; leafId: DockNodeId; index: number }  // without-moving-tab index space
  | { kind: "split"; leafId: DockNodeId; edge: "left" | "right" | "top" | "bottom" };
```

Pure functions beside the types: `insertPanel`, `removePanel`, `movePanel`, `splitLeaf`,
`reorderTab`, `normalize` (delete empty leaves that are neither `locked` nor `persistent`,
collapse single-child branches, merge same-orientation nesting), `validate` (drop unknown
panel ids, clamp `activeTab` to membership, prune `lastLocation` entries whose leaf no
longer exists; structural failure ⇒ `null` ⇒ caller falls back to the default factory), and
the `openPanel` resolution chain (below) as a pure function.

**Interim-leaf contract (until phase 07):** the default factory produces exactly three
well-known leaves, all `persistent`:

- `leaf:leftBottom` — `{ tabs: ["inspector", "environment", "render"], activeTab: "inspector" }`
- `leaf:right` — empty
- `leaf:bottom` — empty

`persistent` means `normalize` never deletes them; **emptiness** (`tabs.length === 0`) is
what drives Layout's conditional region unmount, exactly replacing today's
`rightTools.length`/`bottomTools.length` checks. Reveal-band drops (phase 06), `lastLocation`,
and `defaultLeafId` therefore always resolve: their target ids exist even while a region is
unmounted. Phase 07 removes the flags and lets `normalize` collapse empties.

### 2. Tests — the repo's first editor unit tests

`state/dockLayout.test.ts` on `bun test`: every mutation; `normalize` incl. persistent-leaf
skips, single-child collapse, same-orientation merge; `validate` incl. dangling
`lastLocation` pruning and unknown-id drops; the `openPanel` chain incl. the terminal
fallback; `reorderTab` index semantics matching `insertionIndexForPointer`'s
without-moving-tab space. Add a `"test": "bun test"` script to `editor/package.json` and run
it in `tools/ci/check.sh`'s frontend step (before `bun run build`).

### 3. Store slice

```ts
dockLayout: DockLayout;
lastLocation: Partial<Record<DockPanelId, DockNodeId>>;   // Unreal-style re-open memory
openPanel(id: DockPanelId): void;     // focus-or-open, resolution chain below
closePanel(id: DockPanelId): void;    // active falls back to the index-1 neighbor
activatePanel(id: DockPanelId): void; // open panels only
movePanel(id: DockPanelId, target: DropTarget): void;
reorderTab(leafId: DockNodeId, id: DockPanelId, index: number): void;
setBranchSizes(branchId: DockNodeId, sizes: Record<string, number>): void;
resetDockLayout(): void;
// selector: isPanelOpen(id)
```

**`openPanel` resolution chain** (each step validated): already open ⇒ activate. Else
`lastLocation[id]` if that leaf exists and is not locked ⇒ the panel's default leaf if it
exists (pre-phase-07 it always does — persistent) ⇒ first non-locked leaf in tree order ⇒
terminal fallback: append a fresh leaf as the root branch's last child and insert there
(`splitLeaf` generalized to accept the root id with a trailing edge). The pure resolver
takes the panel→default-leaf mapping as a parameter; this phase ships it as an interim
const beside the types (`DEFAULT_LEAF: Record<DockPanelId, DockNodeId>` —
inspector/environment/render → `leaf:leftBottom`, stats/profiler/material → `leaf:right`,
timeline → `leaf:bottom`), which the phase-05 registry's `defaultLeafId` column absorbs.
`lastLocation` is written by `closePanel` from day one (`movePanel` joins in phase 06).
Deep-links and Topbar buttons all use `openPanel` — focus-or-open, never the silently-no-op
`activatePanel`.

Delete the five old fields + three action trios. `ViewTab`/`moveViewTab` untouched — the
disjoint id spaces are the type-level half of requirement 4.

### 4. Centralized settle subscriber (lands here, not later)

One module-level store subscription on `dockLayout` identity. The store uses plain
`create` (no `subscribeWithSelector` middleware), so use the vanilla listener form with a
manual identity diff — the existing pattern at `TimelinePanel.tsx:205` and
`FrameTimeGraph.tsx:197`:

```ts
useEditorStore.subscribe((s, prev) => {
  if (s.dockLayout !== prev.dockLayout) {
    requestAnimationFrame(() => emitLayoutSettled({ force: true }));
  }
});
```

It subsumes the two existing re-glue paths this migration would otherwise orphan —
`LeftBottomTabs`'s tab-change rAF emit (`Layout.tsx:259-262`) and the `bottomToolsOpen`
effect (`Layout.tsx:88-91`, keyed on lengths that no longer exist) — retire both in this
same change. Region emptiness now lives inside `dockLayout`, so the subscriber fires on
exactly the same transitions, and on every future mutation (drops, opens, closes, resets,
loads) for free. Over-emitting is safe (debounced end tier in `ViewportPanel`). The rrp
`onLayoutChanged` chains and the pixel-resizer pointerup emit (`Layout.tsx:122`) stay until
phase 07.

### 5. Migrate every caller

Topbar/AlarmBadge → `openPanel(...)`; HierarchyTree deep-link → `openPanel("inspector")`;
Layout selectors → leaf-emptiness selectors; LeftBottomTabs/RightSidebar/BottomDock read
their leaf's `tabs`/`activeTab` and call `activatePanel` (their tabs are open by
definition) / `closePanel`; metrics gate → `isPanelOpen("stats")` (polling keyed on
*open*, not active — `display:none` stays free).

## Verify

- `bun test` (new) + `cd editor && bun run check`; `make prepare-for-commit` clean;
  `tools/ci/check.sh` frontend step runs the tests.
- Zero visual change. Manual via `make run`:
  - All three strips behave as before (open/close/switch; right sidebar and bottom dock
    appear/disappear on first-open/last-close).
  - Hierarchy SubRow deep-link still lands on Inspector; AlarmBadge still opens Stats.
  - **Viewport re-glue:** switching left tabs and opening/closing the bottom dock still
    re-glues the subsurface (the two retired emitters' transitions, now via the subscriber).
