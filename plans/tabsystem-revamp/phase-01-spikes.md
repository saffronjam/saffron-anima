# Phase 01 — go/no-go spikes

**Status:** NOT STARTED

## Goal

De-risk the two load-bearing unknowns *before* any dependent code exists: (a) can
`react-resizable-panels` v4 handle a layout tree whose **structure** changes at runtime,
and (b) do the WebKitGTK primitives the drag layer needs actually work in our webview.
Both spikes are throwaway code, deleted within this phase; the findings land as a short
"Spike results" section appended to this plan's `README.md`.

## What exists to build on

- `react-resizable-panels` `^4` (`editor/package.json`) via the shadcn wrapper
  (`components/ui/resizable.tsx`): `ResizablePanelGroup` (orientation prop),
  `ResizablePanel`, `ResizableHandle`, `useDefaultLayout` persistence
  (`app/Layout.tsx:72-81`).
- `devMode` in the store (`state/store.ts`, toggled by the hidden five-click Scene-tab
  gesture, `app/WindowTitlebar.tsx:54-66`) — the natural gate for scratch UI.
- The FLIP settle already proves WAAPI `element.animate` works in this webview
  (`WindowTitlebar.tsx:154-159`), but not *during* an active pointer capture.

## Work

### 1. Spike A — rrp v4 dynamic structure

A dev-gated scratch component (e.g. `app/DockSpike.tsx`, mounted behind `devMode` from
`App.tsx`) that drives one `ResizablePanelGroup` through the operations the dock tree will
need:

- Add and remove children at runtime (simulating a leaf collapsing via `normalize` and a
  drop creating one). Verify remaining panels reclaim space sanely and `defaultSize` /
  imperative sizing behaves on the *changed* set.
- Reconcile sizes after a structural change: drive sizes from controlled state (the future
  `DockBranch.sizes`), confirm `onLayoutChanged` round-trips, and check whether an
  imperative handle (`setLayout` on the group, if exposed in v4) or a keyed remount is
  needed after add/remove.
- Pixel `minSize` fidelity: a px-min panel (the future viewport leaf, 520 px;
  `VIEWPORT_MIN_WIDTH`, `app/Layout.tsx:48`) inside nested groups at a 1280×720 window —
  the cannot-collapse-while-attaching guarantee must be expressible.
- Nested groups three deep (branch → branch → leaf), since orientation alternates per depth.

**Go/no-go:** if structural add/remove cannot be made glitch-free even with a keyed-remount
fallback (key the group subtree by a structure hash, then rAF-force `emitLayoutSettled`),
record it and switch the plan to the dockview-as-engine plan B (see README) before phase 03.
The structure hash is a stable string of the subtree's ordered `(nodeId, node type,
orientation, children)` tuples — **sizes excluded** — used as the `ResizablePanelGroup`'s
React key: child add/remove/reorder remounts the group, resizes never do.

### 2. Spike B — WebKitGTK drag primitives

Same scratch surface, three checks:

- **Portal reparent:** render a stateful component (`useState` counter + an `<img>`) via
  `createPortal` into a host div; `appendChild` the host div across two parents on a button
  click. Verify React state, the image (no reload flash), and a scroll position survive.
- **`document.elementFromPoint` during pointer capture:** `setPointerCapture` on one element,
  and on every `pointermove` hit-test under the cursor across other elements; confirm correct
  results while a `pointer-events: none` ghost follows the cursor (the ghost must never be
  the hit result).
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
