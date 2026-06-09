/// The default Profiler view: a static per-pass GPU table, one row per pass name (occurrences
/// folded and averaged per frame), sorted by GPU ms and graded by the same `passStatus` the HUD's
/// per-pass bars use (so the two read as one system). "% of span" is the share of the frame's
/// begin..end span — NOT a sum, since passes overlap on the GPU.
import { useMemo } from "react";
import { useEditorStore } from "../state/store";
import { passStatus, STATUS_BG, STATUS_TEXT } from "../lib/perfThresholds";
import type { PipelineStatsDto } from "../protocol";
import { cn } from "@/lib/utils";

interface Row {
  name: string; // the pass name — the grouping key (one row per distinct pass)
  gpuMs: number; // average GPU time per captured frame
  occurrences: number; // how many spans were folded in (≈ frames × invocations-per-frame)
  stats?: PipelineStatsDto;
}

/// Sum two pipeline-stat records field-wise. Ratios in `statsLine` divide summed counts, so the
/// folded line reads as the occurrence-weighted average (e.g. total frags / total pixels).
function addStats(a: PipelineStatsDto | undefined, b: PipelineStatsDto): PipelineStatsDto {
  if (a === undefined) {
    return { ...b };
  }
  return {
    inputVertices: a.inputVertices + b.inputVertices,
    vertexInvocations: a.vertexInvocations + b.vertexInvocations,
    clippingInvocations: a.clippingInvocations + b.clippingInvocations,
    clippingPrimitives: a.clippingPrimitives + b.clippingPrimitives,
    fragmentInvocations: a.fragmentInvocations + b.fragmentInvocations,
    computeInvocations: a.computeInvocations + b.computeInvocations,
    pixels: a.pixels + b.pixels,
  };
}

/// Compact count: 1234567 -> "1.2M". Pipeline-stat invocation counts get large.
function formatCount(n: number): string {
  if (n >= 1e9) return `${(n / 1e9).toFixed(1)}B`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(1)}M`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(1)}K`;
  return String(n);
}

/// The derived optimization ratios for one pass: overdraw (fragments / pixels), culling
/// (primitives discarded at clip), vertex reuse (post-transform cache), compute invocations.
function statsLine(s: PipelineStatsDto): string {
  const parts: string[] = [];
  if (s.pixels > 0 && s.fragmentInvocations > 0) {
    parts.push(`overdraw ${(s.fragmentInvocations / s.pixels).toFixed(1)}×`);
  }
  if (s.clippingInvocations > 0) {
    const culled = Math.max(0, 1 - s.clippingPrimitives / s.clippingInvocations);
    parts.push(`cull ${(culled * 100).toFixed(0)}%`);
  }
  if (s.inputVertices > 0) {
    const reuse = Math.max(0, 1 - s.vertexInvocations / s.inputVertices);
    parts.push(`reuse ${(reuse * 100).toFixed(0)}%`);
  }
  if (s.computeInvocations > 0) {
    parts.push(`compute ${formatCount(s.computeInvocations)} inv`);
  }
  return parts.join(" · ");
}

/// Shared column template so the header and every data row line up (each row is its own grid, so
/// fixed numeric tracks — not `auto` — are what keep the values under their labels).
const GRID_COLS = "grid grid-cols-[minmax(0,1fr)_3rem_3rem_3.5rem] items-baseline gap-x-2";

export function CaptureTable() {
  const capture = useEditorStore((s) => s.capture);

  const { rows, spanMs, budgetMs, frameCount } = useMemo(() => {
    const gpu = (capture?.spans ?? []).filter((s) => s.lane === "gpu");
    if (gpu.length === 0) {
      return { rows: [] as Row[], spanMs: 0, budgetMs: 0, frameCount: 1 };
    }
    let minStart = gpu[0].startNs;
    let maxEnd = gpu[0].endNs;
    for (const s of gpu) {
      if (s.startNs < minStart) minStart = s.startNs;
      if (s.endNs > maxEnd) maxEnd = s.endNs;
    }
    const frames = Math.max(1, capture?.metadata.frameCount ?? 1);
    // Per-frame averages: the capture may span many frames, so every metric is divided by the
    // frame count to read as one representative frame (% budget is per-frame; % span is unaffected
    // since the span scales with the frame count too).
    const span = Math.max(0, maxEnd - minStart) / 1e6 / frames;
    const fps = capture?.metadata.targetFps ?? 60;
    const budget = fps > 0 ? 1000 / fps : 0;

    // Fold every occurrence of a pass into one row keyed by name — a multi-frame capture records
    // each pass once per frame, so the raw list is N copies of the same passes.
    const groups = new Map<string, Row>();
    for (const s of gpu) {
      const ms = Math.max(0, s.endNs - s.startNs) / 1e6;
      const g = groups.get(s.name);
      if (g === undefined) {
        groups.set(s.name, {
          name: s.name,
          gpuMs: ms,
          occurrences: 1,
          stats: s.pipelineStats,
        });
      } else {
        g.gpuMs += ms;
        g.occurrences += 1;
        if (s.pipelineStats !== undefined) {
          g.stats = addStats(g.stats, s.pipelineStats);
        }
      }
    }
    const list = [...groups.values()]
      .map((g) => ({ ...g, gpuMs: g.gpuMs / frames }))
      .sort((a, b) => b.gpuMs - a.gpuMs);
    return { rows: list, spanMs: span, budgetMs: budget, frameCount: frames };
  }, [capture]);

  if (rows.length === 0) {
    return (
      <p className="px-1 py-2 text-[11px] italic text-muted-foreground">
        No GPU passes in this capture.
      </p>
    );
  }

  return (
    <div className="flex flex-col gap-1">
      <div className="flex justify-end px-1 text-[10px] text-muted-foreground">
        <span className="whitespace-nowrap font-mono tabular-nums">
          {frameCount > 1 ? "avg frame" : "span"} {spanMs.toFixed(2)} ms
        </span>
      </div>
      <div
        className={cn(GRID_COLS, "px-1 text-[9px] uppercase tracking-wide text-muted-foreground")}
      >
        <span>Pass</span>
        <span className="text-right">GPU ms</span>
        <span className="text-right">% span</span>
        <span className="text-right">% budget</span>
      </div>
      {rows.map((row) => {
        const status = passStatus(row.gpuMs, budgetMs);
        const spanShare = spanMs > 0 ? row.gpuMs / spanMs : 0;
        const budgetShare = budgetMs > 0 ? row.gpuMs / budgetMs : 0;
        const perFrame = row.occurrences / frameCount;
        return (
          <div key={row.name} className="flex flex-col gap-0.5 px-1 py-0.5">
            <div className={GRID_COLS}>
              <span className="truncate text-[11px] text-foreground">
                {row.name}
                {perFrame > 1.5 ? (
                  <span className="ml-1 font-mono text-[9px] text-muted-foreground">
                    ×{Math.round(perFrame)}/frame
                  </span>
                ) : null}
              </span>
              <span
                className={cn("text-right font-mono text-[11px] tabular-nums", STATUS_TEXT[status])}
              >
                {row.gpuMs.toFixed(2)}
              </span>
              <span className="text-right font-mono text-[10px] tabular-nums text-muted-foreground">
                {(spanShare * 100).toFixed(0)}%
              </span>
              <span className="text-right font-mono text-[10px] tabular-nums text-muted-foreground">
                {(budgetShare * 100).toFixed(0)}%
              </span>
            </div>
            <div className="h-1 w-full overflow-hidden rounded-full bg-white/10">
              <div
                className={cn("h-full rounded-full", STATUS_BG[status])}
                style={{ width: `${Math.min(100, budgetShare * 100)}%` }}
              />
            </div>
            {row.stats !== undefined && statsLine(row.stats) !== "" ? (
              <span className="font-mono text-[9px] tabular-nums text-muted-foreground">
                {statsLine(row.stats)}
              </span>
            ) : null}
          </div>
        );
      })}
    </div>
  );
}
