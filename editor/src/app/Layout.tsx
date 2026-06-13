/// The Scene dockspace: a recursive `DockRoot` renders the Scene dock tree — a horizontal
/// [left sidebar | center column | right dock] over the center's vertical
/// [viewport | assets | bottom dock]. Every region is a dock leaf: drag a tab to retab or
/// split it; an empty right/bottom region collapses and the viewport reclaims the space.
/// The per-project layout loads on mount (App remounts this component per project via its
/// `key`). The viewport leaf is the only host the engine paints over — it is `locked` (no
/// strip, no drops) and keeps the live subsurface; reveal bands stand in for the empty
/// right/bottom regions during a torn drag so a panel can be dropped back into them.
import { useEffect } from "react";
import { useEditorStore } from "../state/store";
import { isLeaf } from "../state/dockLayout";
import { DockRoot } from "@/components/dock/DockRoot";
import { useDockDrag } from "@/components/dock/dockDrag";
import { logRender } from "../lib/renderLog";

export function Layout() {
  logRender("Layout");
  const playState = useEditorStore((s) => s.playState);

  // Load the per-project dock trees on mount; Layout remounts per project via its `key`, so
  // this hydrates once per project and no-ops without a loaded project.
  useEffect(() => {
    useEditorStore.getState().hydrateDockLayouts();
  }, []);

  // Play-mode tint: an amber inset ring around the whole dock marks the editor as live
  // (Unity's playmode-tint lesson). The viewport interior stays untinted; it is the game view.
  const playRing = playState === "edit" ? "" : "ring-2 ring-inset ring-amber-500/60 rounded-sm";

  return (
    <div className={`relative flex min-h-0 min-w-0 flex-1 ${playRing}`}>
      <RevealBands />
      <div className="min-h-0 min-w-0 flex-1">
        <DockRoot space="scene" />
      </div>
    </div>
  );
}

/// During a torn dock drag, thin edge bands stand in for the right/bottom regions while they
/// are empty (and thus collapsed) — dropping there docks the tab into the well-known
/// persistent leaf (which always exists in the model), so the region re-expands. The bands
/// carry `data-dock-leaf` so the drag registry picks them up like any mounted leaf.
function RevealBands() {
  const dragging = useDockDrag() !== null;
  const rightEmpty = useEditorStore((s) => {
    const leaf = s.dockLayouts.scene.nodes["leaf:right"];
    return isLeaf(leaf) && leaf.tabs.length === 0;
  });
  const bottomEmpty = useEditorStore((s) => {
    const leaf = s.dockLayouts.scene.nodes["leaf:bottom"];
    return isLeaf(leaf) && leaf.tabs.length === 0;
  });
  if (!dragging) {
    return null;
  }
  return (
    <>
      {rightEmpty && (
        <div
          data-dock-leaf="leaf:right"
          data-dock-accepts-splits="false"
          className="pointer-events-none absolute right-0 top-0 z-20 h-full w-10 border border-dashed border-primary/40 bg-primary/5"
        />
      )}
      {bottomEmpty && (
        <div
          data-dock-leaf="leaf:bottom"
          data-dock-accepts-splits="false"
          className="pointer-events-none absolute bottom-0 left-0 right-0 z-20 h-10 border border-dashed border-primary/40 bg-primary/5"
        />
      )}
    </>
  );
}
