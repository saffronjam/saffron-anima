/// The single client-side mirror of the engine's `PerfConfig` colour scheme. The graph,
/// the headline numbers, the per-pass bars, and the VRAM gauge all grade values through
/// here, so green/amber/red means the same thing across the HUD, the engine, and the
/// e2e tests — never hardcode a threshold in a component.
import type { PerfConfigDto } from "../protocol";

export type PerfStatus = "green" | "amber" | "red";

/// Grade a frame time against the budget and the running median (the engine's rule):
/// 🟢 within budget with headroom and consistent; 🟡 near budget or a moderate spike;
/// 🔴 over budget, a hard spike (> 2× median), or the frozen band.
export function frameTimeStatus(ms: number, config: PerfConfigDto, medianMs: number): PerfStatus {
  const budget = config.budgetMs;
  if (
    ms > budget ||
    ms > config.frozenMs ||
    (medianMs > 0 && ms > config.amberMedianMul * medianMs)
  ) {
    return "red";
  }
  if (
    ms >= config.greenBudgetFrac * budget ||
    (medianMs > 0 && ms > config.greenMedianMul * medianMs)
  ) {
    return "amber";
  }
  return "green";
}

/// Grade a VRAM usage fraction: ≥ 100% or ≥ crit → red, ≥ warn → amber, else green.
export function vramStatus(fraction: number, config: PerfConfigDto): PerfStatus {
  if (fraction >= 1 || fraction >= config.vramCritFrac) {
    return "red";
  }
  if (fraction >= config.vramWarnFrac) {
    return "amber";
  }
  return "green";
}

/// Grade one pass's share of the frame budget. Per-pass soft budgets are not yet in the
/// engine config, so this uses a single share: > 50% of budget = red, > 25% = amber.
export function passStatus(ms: number, budgetMs: number): PerfStatus {
  if (budgetMs <= 0) {
    return "green";
  }
  const fraction = ms / budgetMs;
  if (fraction > 0.5) {
    return "red";
  }
  if (fraction > 0.25) {
    return "amber";
  }
  return "green";
}

/// Tailwind text-colour class per status, for DOM labels.
export const STATUS_TEXT: Record<PerfStatus, string> = {
  green: "text-emerald-400",
  amber: "text-amber-400",
  red: "text-red-400",
};

/// Tailwind background class per status, for bars and dots.
export const STATUS_BG: Record<PerfStatus, string> = {
  green: "bg-emerald-500",
  amber: "bg-amber-500",
  red: "bg-red-500",
};

/// Fixed hex palette (the app is dark-only) for the uPlot canvas, which cannot reliably
/// take the oklch theme tokens as canvas colours.
export const GRAPH_COLORS = {
  total: "#e5e7eb",
  cpu: "#60a5fa",
  gpu: "#c084fc",
  budget: "#f59e0b",
  frozen: "#ef4444",
  grid: "rgba(255,255,255,0.07)",
  axis: "rgba(255,255,255,0.45)",
} as const;
