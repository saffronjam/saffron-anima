/// The torn-drag visuals: a cursor-following mini-tab ghost and a single highlight over the
/// hovered leaf (VS Code's one-overlay pattern). Both subscribe to `dockDrag` and are
/// `pointer-events: none`, so they never become the hit-test result (w3c/pointerevents#566
/// — the ghost must never resolve under the cursor). Mounted once at app root.
import { createPortal } from "react-dom";
import type { DropTarget } from "../../state/dockLayout";
import { useDockDrag } from "./dockDrag";

export function DockDropOverlay() {
  const drag = useDockDrag();
  if (!drag) {
    return null;
  }
  return (
    <>
      {drag.hovered && <DropHighlight target={drag.hovered} />}
      <DragGhost label={drag.ghostLabel} x={drag.pointer.x} y={drag.pointer.y} />
    </>
  );
}

/// Fills the hovered leaf (or reveal band) — the merge/insert preview; for an edge-split
/// target it fills the resulting half.
function DropHighlight({ target }: { target: DropTarget }) {
  const el = document.querySelector<HTMLElement>(`[data-dock-leaf="${target.leafId}"]`);
  if (!el) {
    return null;
  }
  const rect = el.getBoundingClientRect();
  const inset = splitInset(target, rect);
  return createPortal(
    <div
      className="pointer-events-none fixed z-[90] rounded-sm bg-primary/15 ring-1 ring-primary transition-[top,left,width,height] duration-100"
      style={inset}
    />,
    document.body,
  );
}

function splitInset(
  target: DropTarget,
  rect: DOMRect,
): { left: number; top: number; width: number; height: number } {
  if (target.kind === "tab") {
    return { left: rect.left, top: rect.top, width: rect.width, height: rect.height };
  }
  const half = (n: number) => n / 2;
  switch (target.edge) {
    case "left":
      return { left: rect.left, top: rect.top, width: half(rect.width), height: rect.height };
    case "right":
      return {
        left: rect.left + half(rect.width),
        top: rect.top,
        width: half(rect.width),
        height: rect.height,
      };
    case "top":
      return { left: rect.left, top: rect.top, width: rect.width, height: half(rect.height) };
    case "bottom":
      return {
        left: rect.left,
        top: rect.top + half(rect.height),
        width: rect.width,
        height: half(rect.height),
      };
  }
}

// Sized to its label (capped at 18rem) so a panel name reads in full — the dragged tab can be
// far narrower than its title, so the ghost must not inherit the tab width.
function DragGhost({ label, x, y }: { label: string; x: number; y: number }) {
  return createPortal(
    <div
      className="pointer-events-none fixed z-[100] flex h-8 max-w-[18rem] items-center rounded-md border border-border bg-card px-2.5 text-xs font-medium text-foreground shadow-lg"
      style={{ left: x + 10, top: y + 10 }}
    >
      <span className="min-w-0 truncate">{label}</span>
    </div>,
    document.body,
  );
}
