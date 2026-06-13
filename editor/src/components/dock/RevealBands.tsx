/// During a torn dock drag, thin edge bands stand in for a dockspace's empty (collapsed) edge
/// regions — dropping there docks the tab into the well-known persistent leaf (which always exists
/// in the model), so the region re-expands. The bands carry `data-dock-leaf` so the drag registry
/// picks them up like any mounted leaf, and `pointer-events-none` so they never self-hit the manual
/// rect hit-test. Parameterized by dockspace so the Scene and asset-editor islands share it.
import { useShallow } from "zustand/react/shallow";
import { useEditorStore } from "../../state/store";
import { isLeaf, type DockNodeId, type DockSpaceKind } from "../../state/dockLayout";
import { useDockDrag } from "./dockDrag";

export interface RevealBand {
  leafId: DockNodeId;
  edge: "left" | "right" | "bottom";
}

const BAND_POSITION: Record<RevealBand["edge"], string> = {
  left: "left-0 top-0 h-full w-10",
  right: "right-0 top-0 h-full w-10",
  bottom: "bottom-0 left-0 right-0 h-10",
};

export function RevealBands({ space, bands }: { space: DockSpaceKind; bands: RevealBand[] }) {
  const dragging = useDockDrag() !== null;
  // One selector returns the empty-state of each band's leaf. `bands` is a stable-length constant
  // per call site, so the hook count never varies; `useShallow` keeps the boolean array stable.
  const empty = useEditorStore(
    useShallow((s) => {
      const layout = s.dockLayouts[space];
      return bands.map((band) => {
        const leaf = layout.nodes[band.leafId];
        return isLeaf(leaf) && leaf.tabs.length === 0;
      });
    }),
  );
  if (!dragging) {
    return null;
  }
  return (
    <>
      {bands.map((band, i) =>
        empty[i] ? (
          <div
            key={band.leafId}
            data-dock-leaf={band.leafId}
            data-dock-accepts-splits="false"
            className={`pointer-events-none absolute z-20 border border-dashed border-primary/40 bg-primary/5 ${BAND_POSITION[band.edge]}`}
          />
        ) : null,
      )}
    </>
  );
}
