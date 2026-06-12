# Phase 02 — shared tab strip + drag hook; titlebar retrofitted

**Status:** NOT STARTED

## Goal

Extract the titlebar's tab-drag machine into a reusable hook + component pair —
`useTabStripDrag` and `TabStrip` — and make `WindowTitlebar` consume them. One
implementation, two size variants. Parity by copy rots; after this phase there is exactly
one drag machine in the tree, and requirement 3 ("all main-tab drag functionality") is
satisfiable for free everywhere `TabStrip` is used.

## What exists to build on

The whole machine lives in `app/WindowTitlebar.tsx`:

- `TabDragState` (`:27-37`): `{id, startX, currentX, dragging, startIndex, previewIndex,
  width, order, centers}` — centers snapshotted **once** at drag start.
- `beginTabDrag` (`:188-226`): left button only, bails on non-closable tabs and on presses
  inside `[data-tab-close='true']` (`:193`), `setPointerCapture`, snapshots order + centers.
- `moveTabDrag` (`:228-242`): latches `dragging` at `TAB_DRAG_THRESHOLD_PX = 4` (`:22`),
  recomputes `previewIndex` via `insertionIndexForPointer` (`:170-186`, which skips the
  pinned `"scene"` id at `:174`).
- `tabStyle` (`:268-291`): dragged tab follows the pointer (`translateX(currentX - startX)`),
  displaced neighbors shift by `±width` — transform-only, model untouched until drop.
- `endTabDrag` (`:244-266`): click-vs-drag — no threshold crossed ⇒ activate; dragged ⇒
  snapshot pre-drop lefts into `settleRef` and commit `moveViewTab`.
- The FLIP settle (`useLayoutEffect`, `:130-160`): suppress transitions, diff pre-drop lefts
  against final rects, WAAPI `animate(translateX(diff) → none, 150ms ease-out)`.
- Reset on `onPointerCancel` (`:317`). There is **no** `lostpointercapture` handler today.
- Titlebar-specific layers that must stay local: the dev-mode gesture (`:54-66`), the
  `data-titlebar-control` fencing so `appWindow.startDragging()` never fires mid-gesture
  (`:102-118`), `tabIcon` (`:425-442`), the close X, `moveViewTab`'s index clamping
  (`store.ts:517-530`).

## Work

### 1. `components/dock/useTabStripDrag.ts`

The machine above, verbatim, parameterized:

```ts
interface UseTabStripDragOptions {
  domain: "view" | "dock";          // tear-out (phase 06) exists only for "dock"
  pinnedIds?: string[];             // generalizes the hardcoded "scene" skip
  isDraggable?(id: string): boolean;
  shouldIgnoreDragStart?(target: Element): boolean; // default: closest("[data-tab-close='true']")
  onReorder(id: string, index: number): void;       // index in the without-moving-tab space
  onActivate(id: string): void;                     // the pointerup-without-threshold path
  onTearOut?(id: string, pointer: { x: number; y: number }): void; // dock only, wired in phase 06
}
interface UseTabStripDragApi {
  handlersFor(id: string): DOMAttributes<HTMLButtonElement>; // down/move/up/cancel/lostpointercapture
  styleFor(id: string): CSSProperties | undefined;
  registerTabRef(id: string, el: HTMLButtonElement | null): void;
  dragging: boolean;
}
```

Every mechanic preserved exactly: pointer capture + `preventDefault`, the 4 px latch, the
single centers snapshot, neighbor `translateX(±draggedWidth)` preview, click-vs-drag
semantics, the close-affordance fence as the `shouldIgnoreDragStart` default (a press on any
X never arms a drag — titlebar or dock), the WAAPI FLIP settle, and reset on **both**
`pointercancel` and `lostpointercapture` (the one deliberate delta — see gate).
`pinnedIds` are both excluded from insertion targets **and** non-draggable — generalizing
the two mechanics that pin the scene tab today: the `insertionIndexForPointer` skip (`:174`)
and the `beginTabDrag` bail on `!tab.closable` (`:189`).

### 2. `components/dock/TabStrip.tsx`

Props: `{ items: { id, title, icon?, closable }[], activeId, size: "main" | "dock",
onActivate, onClose, drag: Omit<UseTabStripDragOptions, "onActivate"> }` — `TabStrip`
forwards its own `onActivate`/`onClose` into the hook options, so the activation callback
exists once, not twice. Size variants (cva, theme tokens only):

- `main` — the current titlebar look: `h-8 min-w-28 max-w-48 rounded-t-md`, icon + truncated
  label + close X (`WindowTitlebar.tsx:373-422`).
- `dock` — the compact look from `RightSidebar.tsx:47-52`: `h-8 text-xs`, `-mb-px border-b-2`
  active treatment, **`min-w-0` shrink with truncated labels** so an overfull strip degrades
  by shrinking — no scroll offset, so the snapshot-centers math stays valid (the real
  overflow affordance is phase 09).

The `dock` variant is used by nothing yet (phase 04); it ships here so the component is the
unit under test from day one.

### 3. Retrofit `WindowTitlebar`

Consume the hook with `domain: "view"`, `pinnedIds: ["scene"]`, `onReorder: moveViewTab`,
`onActivate: setActiveViewTab` (routing the scene tab through `activateSceneTab` for the dev
gesture), **no `onTearOut`**. The titlebar keeps its local layers (drag-region fencing, dev
gesture, window buttons). Requirement 4's first structural half lands here: the `view`
instance simply has no tear-out path, so a main tab can never leave the strip.

## Verify

- `cd editor && bun run check`; `make prepare-for-commit` clean.
- **Gate: the titlebar is behaviorally identical, with exactly one allowed delta** — the
  added `lostpointercapture` reset. Manual checklist via `make run`:
  - 4 px latch: a sub-threshold press-release activates, never reorders.
  - Drag right/left across neighbors: live preview shifts; drop settles with the FLIP
    animation from under the cursor; no phantom slide on neighbors.
  - Scene tab: not draggable past it (insertion never lands left of Scene), five-click dev
    gesture still toggles dev mode.
  - Close X: click closes, press-drag on the X never starts a drag.
  - A real drag never activates the tab. Capture-loss cancel restores cleanly: trigger
    `lostpointercapture` by switching workspace (Super key) mid-drag, or simulate
    `pointercancel` via devtools — a mouse on the desktop never fires it naturally.
  - Titlebar empty-region drag still moves the window; double-click still maximizes.
