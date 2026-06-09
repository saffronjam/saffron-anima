/// The time-ordered flame chart: a CPU lane (render-thread phases) and a GPU lane (the nested
/// pass tree) under synthetic lane roots, sharing one axis when the capture is correlated.
/// flame-chart-js renders the `{name, start, duration, children}` shape captureTree.ts already
/// produces. Spans are coloured by magnitude (the HUD's `passStatus`); the selected pass is
/// highlighted, and clicking a span drives the shared `selectedPass`.
import { useMemo } from "react";
import { FlameChartComponent, type NodeTypes } from "flame-chart-js/react";
import type { FlameChartNode, FlameChartSettings } from "flame-chart-js";
import { useEditorStore } from "../state/store";
import { spansToFlameTree, type FlameNode } from "../lib/captureTree";
import { passStatus } from "../lib/perfThresholds";

const STATUS_HEX = { green: "#34d399", amber: "#fbbf24", red: "#f87171" } as const;
const LANE_HEX = { cpu: "#60a5fa", gpu: "#c084fc" } as const;
const SELECTED_HEX = "#f5d0fe";

// flame-chart-js renders to a canvas and defaults to a white field + black text. This recolours
// every layer (chart, time grid, top overview, tooltip) onto the editor's dark surfaces — a
// card-grey field rather than pure black, so the bright per-status span colours stay legible.
// Stable module const: the React wrapper re-applies `settings` whenever its reference changes.
const FLAME_DARK: FlameChartSettings = {
  styles: {
    main: {
      backgroundColor: "#1e1e1e",
      fontColor: "#141418",
      headerColor: "#1e1e1e",
      headerStrokeColor: "rgba(255, 255, 255, 0.08)",
      tooltipBackgroundColor: "#262626",
      tooltipHeaderFontColor: "#fafafa",
      tooltipBodyFontColor: "#d4d4d4",
      tooltipShadowColor: "rgba(0, 0, 0, 0.6)",
    },
    timeGrid: { color: "#d4d4d8" },
    timeframeSelectorPlugin: {
      backgroundColor: "#181818",
      fontColor: "#d4d4d8",
      overlayColor: "rgba(0, 0, 0, 0.55)",
      graphFillColor: "rgba(255, 255, 255, 0.06)",
      graphStrokeColor: "rgba(255, 255, 255, 0.22)",
      bottomLineColor: "rgba(255, 255, 255, 0.1)",
      knobColor: "#3a3a3a",
      knobStrokeColor: "rgba(255, 255, 255, 0.3)",
    },
  },
};

export function CaptureFlame() {
  const capture = useEditorStore((s) => s.capture);
  const selectedPass = useEditorStore((s) => s.selectedPass);
  const setSelectedPass = useEditorStore((s) => s.setSelectedPass);
  const correlated = capture?.metadata.correlated ?? true;

  const data = useMemo<FlameChartNode[]>(() => {
    if (capture === null || capture.spans.length === 0) {
      return [];
    }
    const tree = spansToFlameTree(capture.spans);
    const fps = capture.metadata.targetFps || 60;
    const budgetMs = fps > 0 ? 1000 / fps : 0;
    const toNode = (node: FlameNode): FlameChartNode => ({
      name: node.name,
      start: node.start,
      duration: node.duration,
      color:
        node.name === selectedPass ? SELECTED_HEX : STATUS_HEX[passStatus(node.duration, budgetMs)],
      children: node.children.map(toNode),
    });
    const laneRoot = (name: string, color: string, forest: FlameNode[]): FlameChartNode | null => {
      if (forest.length === 0) {
        return null;
      }
      let min = forest[0].start;
      let max = forest[0].start + forest[0].duration;
      for (const root of forest) {
        min = Math.min(min, root.start);
        max = Math.max(max, root.start + root.duration);
      }
      return {
        name,
        start: min,
        duration: Math.max(0.0001, max - min),
        color,
        children: forest.map(toNode),
      };
    };
    return [
      laneRoot("CPU render thread", LANE_HEX.cpu, tree.cpu),
      laneRoot(
        correlated ? "GPU queue" : "GPU queue (uncorrelated — own zero)",
        LANE_HEX.gpu,
        tree.gpu,
      ),
    ].filter((node): node is FlameChartNode => node !== null);
  }, [capture, selectedPass, correlated]);

  if (capture === null || data.length === 0) {
    return (
      <p className="px-1 py-2 text-[11px] italic text-muted-foreground">No capture to chart.</p>
    );
  }

  const onSelect = (selected: NodeTypes): void => {
    if (selected !== null && selected.type === "flame-chart-node") {
      setSelectedPass(selected.node?.source.name ?? null);
    }
  };

  return (
    <div className="h-full min-h-72 w-full overflow-hidden rounded-md border border-border">
      <FlameChartComponent
        data={data}
        settings={FLAME_DARK}
        className="size-full"
        onSelect={onSelect}
      />
    </div>
  );
}
