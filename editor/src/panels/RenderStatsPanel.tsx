/// The Render Stats panel: the performance-telemetry dashboard plus the render toggles.
///
/// Two timing families that must not be conflated:
///   - "Engine frame" / "GPU" / the percentile graph come from the engine
///     (`render-stats`, `frame-history`, `pass-timings`). GPU + per-pass timing need the
///     profiler in timestamps mode; the frame-time history is always recorded.
///   - "UI poll" / "UI frame" are client-side: the webview reconcile cadence and repaint
///     rate. The native viewport paints independently of the webview.
///
/// Everything is graded through the shared `PerfConfig` (lib/perfThresholds) so the HUD
/// agrees with the engine and the e2e tests. Under a software rasterizer (llvmpipe) the
/// GPU numbers are CPU rasterization time — a banner says so.
import { useCallback, useEffect, useState } from "react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { NumberDrag } from "../components/NumberDrag";
import { FrameTimeGraph } from "../components/FrameTimeGraph";
import { MetricsRefreshControl } from "../components/MetricsRefreshControl";
import {
  frameTimeStatus,
  passStatus,
  vramStatus,
  STATUS_BG,
  STATUS_TEXT,
  type PerfStatus,
} from "../lib/perfThresholds";
import type { AlarmEventDto, RenderStats } from "../protocol";
import { cn } from "@/lib/utils";
import { Label } from "@/components/ui/label";
import { Separator } from "@/components/ui/separator";
import { ScrollArea } from "@/components/ui/scroll-area";
import { Switch } from "@/components/ui/switch";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";

type AaMode = RenderStats["aa"];

const AA_MODES: { value: AaMode; label: string }[] = [
  { value: "off", label: "Off" },
  { value: "fxaa", label: "FXAA" },
  { value: "taa", label: "TAA" },
  { value: "msaa2", label: "MSAA 2x" },
  { value: "msaa4", label: "MSAA 4x" },
  { value: "msaa8", label: "MSAA 8x" },
];

const TARGET_FPS = [30, 60, 90, 120, 144];

/// The boolean feature toggles (label + the stat field + its setter). RT-gated rows
/// carry `rtGated` so the panel disables them when the device lacks support.
const TOGGLES: {
  label: string;
  field: keyof RenderStats;
  set: (on: boolean) => Promise<unknown>;
  rtGated?: boolean;
}[] = [
  { label: "Clustered", field: "clustered", set: (on) => client.setClustered(on) },
  { label: "Depth Pre-pass", field: "depthPrepass", set: (on) => client.setDepthPrepass(on) },
  { label: "Shadows", field: "shadows", set: (on) => client.setShadows(on) },
  { label: "IBL", field: "ibl", set: (on) => client.setIbl(on) },
  { label: "SSAO", field: "ssao", set: (on) => client.setSsao(on) },
  { label: "Contact Shadows", field: "contactShadows", set: (on) => client.setContactShadows(on) },
  { label: "SSGI", field: "ssgi", set: (on) => client.setSsgi(on) },
  { label: "DDGI", field: "ddgi", set: (on) => client.setGi(on ? "ddgi" : "off") },
  { label: "RT Shadows", field: "rtShadows", set: (on) => client.setRtShadows(on), rtGated: true },
  { label: "ReSTIR", field: "restir", set: (on) => client.setRestir(on), rtGated: true },
];

function Stat({ label, value }: { label: string; value: React.ReactNode }) {
  return (
    <div className="flex items-baseline justify-between gap-2">
      <span className="text-[11px] text-muted-foreground">{label}</span>
      <span className="font-mono text-[11px] tabular-nums text-foreground">{value}</span>
    </div>
  );
}

/// A labelled metric whose value is coloured by its perf status.
function Metric({ label, value, status }: { label: string; value: string; status?: PerfStatus }) {
  return (
    <div className="flex items-baseline justify-between gap-2">
      <span className="text-[11px] text-muted-foreground">{label}</span>
      <span
        className={cn(
          "font-mono text-[11px] tabular-nums",
          status ? STATUS_TEXT[status] : "text-foreground",
        )}
      >
        {value}
      </span>
    </div>
  );
}

/// A thin status-coloured fill bar (budget headroom, VRAM, per-pass share).
function Bar({ fraction, status }: { fraction: number; status: PerfStatus }) {
  const pct = Math.max(0, Math.min(1, fraction)) * 100;
  return (
    <div className="h-1.5 w-full overflow-hidden rounded-full bg-white/10">
      <div className={cn("h-full rounded-full", STATUS_BG[status])} style={{ width: `${pct}%` }} />
    </div>
  );
}

function ToggleRow({
  label,
  checked,
  disabled,
  tooltip,
  onCheckedChange,
}: {
  label: string;
  checked: boolean;
  disabled: boolean;
  tooltip?: string;
  onCheckedChange(next: boolean): void;
}) {
  const row = (
    <div className="grid grid-cols-[1fr_auto] items-center gap-1.5">
      <Label className="truncate text-[11px] font-normal text-muted-foreground">{label}</Label>
      <Switch checked={checked} disabled={disabled} onCheckedChange={onCheckedChange} />
    </div>
  );
  if (!tooltip) {
    return row;
  }
  return (
    <Tooltip>
      <TooltipTrigger asChild>
        <div>{row}</div>
      </TooltipTrigger>
      <TooltipContent>{tooltip}</TooltipContent>
    </Tooltip>
  );
}

const SEVERITY_DOT: Record<AlarmEventDto["severity"], string> = {
  info: "bg-muted-foreground",
  warning: "bg-amber-500",
  critical: "bg-red-500",
};

function formatMb(bytes: number): string {
  return `${(bytes / (1024 * 1024)).toFixed(0)} MB`;
}

interface StatsSnapshot {
  stats: RenderStats | null;
  pollRateHz: number;
  uiFrameRateHz: number;
  uiFrameMs: number;
}

function readStatsSnapshot(): StatsSnapshot {
  const s = useEditorStore.getState();
  return {
    stats: s.renderStats,
    pollRateHz: s.pollRateHz,
    uiFrameRateHz: s.uiFrameRateHz,
    uiFrameMs: s.uiFrameMs,
  };
}

/// Read the high-frequency engine/UI readouts (`render-stats`, the poll rate, the UI frame
/// rate) from the store on the auto-refresh interval — via `getState`, **not** a reactive
/// `useEditorStore` selector. Those fields are rewritten at the 20 Hz fast-reconcile rate; a
/// reactive subscription would re-render this whole (large) panel 20 times a second regardless
/// of pause, halving the webview frame rate. Polling instead re-renders only at the refresh
/// rate, and freezes completely while paused. `refresh()` forces an immediate re-read so an
/// optimistic control write (a toggle, AA, exposure) still shows at once, even while paused.
function useThrottledStats(paused: boolean, intervalMs: number): [StatsSnapshot, () => void] {
  const [snapshot, setSnapshot] = useState<StatsSnapshot>(readStatsSnapshot);
  const refresh = useCallback(() => setSnapshot(readStatsSnapshot()), []);
  useEffect(() => {
    if (paused) {
      return;
    }
    refresh();
    const id = window.setInterval(refresh, intervalMs);
    return () => window.clearInterval(id);
  }, [paused, intervalMs, refresh]);
  return [snapshot, refresh];
}

export function RenderStatsPanel() {
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const perfConfig = useEditorStore((s) => s.perfConfig);
  const frameHistory = useEditorStore((s) => s.frameHistory);
  const passTimings = useEditorStore((s) => s.passTimings);
  const alarmLog = useEditorStore((s) => s.alarmLog);
  const metricsPaused = useEditorStore((s) => s.metricsPaused);
  const metricsRefreshMs = useEditorStore((s) => s.metricsRefreshMs);
  const metricsRangeSec = useEditorStore((s) => s.metricsRangeSec);
  const metricsBucketMs = useEditorStore((s) => s.metricsBucketMs);
  const setRenderStats = useEditorStore((s) => s.setRenderStats);
  const setPerfConfig = useEditorStore((s) => s.setPerfConfig);
  const setDragActive = useEditorStore((s) => s.setDragActive);
  const [error, setError] = useState<string | null>(null);

  // The volatile engine/UI readouts (render-stats, poll rate, UI frame rate) are sampled from
  // the store on the refresh interval rather than subscribed reactively — see useThrottledStats.
  // This is what makes pause actually freeze the panel and keeps it off the 20 Hz reconcile rate.
  const [snap, refreshStats] = useThrottledStats(metricsPaused, metricsRefreshMs);
  const stats = snap.stats;
  const pollRateHz = snap.pollRateHz;
  const uiFrameRateHz = snap.uiFrameRateHz;
  const uiFrameMs = snap.uiFrameMs;
  const readout = {
    fps: stats?.fps ?? 0,
    frameMs: stats?.frameMs ?? 0,
    gpuMs: stats?.gpuMs ?? 0,
    pollRateHz,
    uiFrameRateHz,
    uiFrameMs,
  };

  const ready = phase === "ready";

  if (!stats) {
    return (
      <div className="flex h-full min-h-0 flex-col">
        <div className="p-3.5 text-center italic text-muted-foreground">
          {ready ? "Waiting for stats…" : "Engine not ready"}
        </div>
      </div>
    );
  }

  // Optimistically fold an echoed flag into the live stats; the next poll re-reads the full
  // bag so derived counters stay accurate. refreshStats() re-samples the snapshot at once so
  // the control reflects the change immediately, even while the auto-refresh is paused.
  const optimistic = (patch: Partial<RenderStats>): void => {
    setRenderStats({ ...stats, ...patch });
    refreshStats();
  };

  const onAa = (mode: AaMode): void => {
    setError(null);
    optimistic({ aa: mode });
    void client
      .setAa(mode)
      .then((res) => optimistic({ aa: res.aa }))
      .catch((err: unknown) => setError(errorText(err)));
  };

  const onToggle = (
    field: keyof RenderStats,
    set: (on: boolean) => Promise<unknown>,
    next: boolean,
  ): void => {
    setError(null);
    const previous = stats[field];
    optimistic({ [field]: next } as Partial<RenderStats>);
    void set(next)
      .then((res) => {
        const echoed = (res as Record<string, unknown>)[field];
        if (typeof echoed === "boolean") {
          optimistic({ [field]: echoed } as Partial<RenderStats>);
        }
      })
      .catch((err: unknown) => {
        optimistic({ [field]: previous } as Partial<RenderStats>);
        setError(errorText(err));
      });
  };

  const onExposure = (ev: number): void => {
    setError(null);
    optimistic({ exposureEv: ev });
    void client
      .setExposure(ev)
      .then((res) => optimistic({ exposureEv: res.exposureEv }))
      .catch((err: unknown) => setError(errorText(err)));
  };

  const onProfiler = (on: boolean): void => {
    setError(null);
    const mode = on ? "timestamps" : "off";
    optimistic({ profilerMode: mode });
    void client
      .setProfilerMode(mode)
      .then((res) => optimistic({ profilerMode: res.mode }))
      .catch((err: unknown) => setError(errorText(err)));
  };

  const onTargetFps = (fps: number): void => {
    setError(null);
    void client
      .setPerfConfig({ targetFps: fps })
      .then((config) => setPerfConfig(config))
      .catch((err: unknown) => setError(errorText(err)));
  };

  const profilerOn = stats.profilerMode !== "off";
  const budgetMs = perfConfig?.budgetMs ?? 0;
  const medianMs = frameHistory?.p50Ms ?? 0;
  // Headline frame time from the windowed history (steady at the ~4 Hz metrics rate)
  // rather than the per-frame EMA (which the 20 Hz render-stats poll makes flicker).
  const frameTimeMs = frameHistory ? frameHistory.meanMs : stats.cpuFrameMs + stats.cpuWaitMs;
  const fillStatus = perfConfig ? frameTimeStatus(frameTimeMs, perfConfig, medianMs) : "green";
  const bound = stats.cpuWaitMs > stats.cpuFrameMs ? "GPU-bound" : "CPU-bound";
  const vramKnown = stats.vramBudgetBytes > 0;
  const vramFrac = vramKnown ? stats.vramUsageBytes / stats.vramBudgetBytes : 0;
  const vramStat = perfConfig && vramKnown ? vramStatus(vramFrac, perfConfig) : "green";
  const lowStatus = (ms: number): PerfStatus | undefined =>
    perfConfig ? frameTimeStatus(ms, perfConfig, medianMs) : undefined;
  const recentAlarms = alarmLog.slice(Math.max(0, alarmLog.length - 8)).reverse();
  // Convert the wall-clock Range + Window (bucket) to frame counts using the windowed-average
  // frame time, not the instantaneous fps: the latter wobbles every 20 Hz poll, which shifts
  // bucket boundaries (visible "shaking"), and it must freeze with the rest of the dashboard
  // on pause. `frameHistory` rides the pausable metrics lane, so a rate from it is steady.
  const avgFps = frameHistory && frameHistory.meanMs > 0 ? 1000 / frameHistory.meanMs : 60;
  const rangeFrames = Math.min(72000, Math.max(1, Math.round(metricsRangeSec * avgFps)));
  const bucketFrames = Math.max(1, Math.round((metricsBucketMs / 1000) * avgFps));
  const bucketSeconds = metricsBucketMs / 1000;

  return (
    <div className="flex h-full min-h-0 flex-col">
      <ScrollArea className="min-h-0 flex-1">
        <div className="flex flex-col gap-2 p-2.5">
          {stats.softwareGpu ? (
            <p className="rounded-sm border border-amber-500/40 bg-amber-500/10 px-2 py-1 text-[10px] leading-snug text-amber-300">
              Software rasterizer (llvmpipe) — GPU timings are CPU rasterization time, not
              representative of hardware.
            </p>
          ) : null}

          {/* Performance controls: the profiler depth + the frame-budget target. */}
          <section className="flex flex-col gap-1.5 rounded-md border border-border bg-background px-2.5 py-2">
            <div className="grid grid-cols-[1fr_auto] items-center gap-1.5">
              <Tooltip>
                <TooltipTrigger asChild>
                  <Label className="truncate text-[11px] font-normal text-muted-foreground">
                    Profiler
                  </Label>
                </TooltipTrigger>
                <TooltipContent>
                  Per-pass GPU timestamps + VMA budget (off keeps the host at baseline cost)
                </TooltipContent>
              </Tooltip>
              <Switch checked={profilerOn} disabled={!ready} onCheckedChange={onProfiler} />
            </div>
            <div className="grid grid-cols-[1fr_auto] items-center gap-1.5">
              <Label className="truncate text-[11px] font-normal text-muted-foreground">
                Target FPS
              </Label>
              <Select
                value={perfConfig ? String(perfConfig.targetFps) : ""}
                disabled={!ready || !perfConfig}
                onValueChange={(v) => onTargetFps(Number(v))}
              >
                <SelectTrigger size="sm" className="h-7 w-[112px] font-mono text-[11px]">
                  <SelectValue placeholder="—" />
                </SelectTrigger>
                <SelectContent>
                  {TARGET_FPS.map((fps) => (
                    <SelectItem key={fps} value={String(fps)} className="text-[11px]">
                      {fps} Hz
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>
          </section>

          {/* Headline frame time: budget-fill bar, the CPU/GPU bottleneck, percentile lows. */}
          <section className="flex flex-col gap-1 rounded-md border border-border bg-background px-2.5 py-2">
            <div className="flex items-baseline justify-between gap-2">
              <span className="text-[11px] text-muted-foreground">Frame</span>
              <span className={cn("font-mono text-[11px] tabular-nums", STATUS_TEXT[fillStatus])}>
                {frameTimeMs.toFixed(2)} ms
                {budgetMs > 0 ? ` / ${budgetMs.toFixed(1)} ms` : ""}
              </span>
            </div>
            <Bar fraction={budgetMs > 0 ? frameTimeMs / budgetMs : 0} status={fillStatus} />
            <Stat label="Bottleneck" value={bound} />
            <Separator className="my-1" />
            <Metric
              label="p50 (median)"
              value={frameHistory ? `${frameHistory.p50Ms.toFixed(2)} ms` : "—"}
              status={frameHistory ? lowStatus(frameHistory.p50Ms) : undefined}
            />
            <Metric
              label="p95 (5% low)"
              value={frameHistory ? `${frameHistory.p95Ms.toFixed(2)} ms` : "—"}
              status={frameHistory ? lowStatus(frameHistory.p95Ms) : undefined}
            />
            <Metric
              label="p99 (1% low)"
              value={frameHistory ? `${frameHistory.p99Ms.toFixed(2)} ms` : "—"}
              status={frameHistory ? lowStatus(frameHistory.p99Ms) : undefined}
            />
            <Metric
              label="p99.9 (0.1% low)"
              value={frameHistory ? `${frameHistory.p999Ms.toFixed(2)} ms` : "—"}
              status={frameHistory ? lowStatus(frameHistory.p999Ms) : undefined}
            />
            <Stat label="Max" value={frameHistory ? `${frameHistory.maxMs.toFixed(2)} ms` : "—"} />
            <Stat label="Stutters" value={frameHistory ? frameHistory.stutterCount : "—"} />
          </section>

          {perfConfig ? (
            <section className="rounded-md border border-border bg-background p-1.5">
              <div className="flex items-center justify-between gap-2 px-1 pb-1">
                <span className="text-[10px] text-muted-foreground">Frame time</span>
                <MetricsRefreshControl />
              </div>
              <FrameTimeGraph
                budgetMs={budgetMs}
                rangeFrames={rangeFrames}
                bucketFrames={bucketFrames}
                bucketSeconds={bucketSeconds}
              />
              <div className="flex items-center justify-end gap-3 px-1 pt-1 text-[10px] text-muted-foreground">
                <LegendDot color="#e5e7eb" label="total" />
                <LegendDot color="#60a5fa" label="cpu" />
                <LegendDot color="#c084fc" label="gpu" />
                <LegendDot color="#f59e0b" label="budget" />
              </div>
            </section>
          ) : null}

          {/* Per-pass GPU breakdown (timestamps mode). Numbers are relative — passes
              overlap on the GPU, so the parts do not cleanly sum to the frame total. */}
          <section className="flex flex-col gap-1 rounded-md border border-border bg-background px-2.5 py-2">
            <div className="flex items-baseline justify-between gap-2">
              <span className="text-[11px] font-medium text-foreground">Per-pass GPU</span>
              {passTimings ? (
                <span className="font-mono text-[10px] tabular-nums text-muted-foreground">
                  span {passTimings.gpuTotalMs.toFixed(2)} ms
                </span>
              ) : null}
            </div>
            {!profilerOn ? (
              <p className="text-[10px] italic text-muted-foreground">
                Enable the profiler for per-pass GPU timing.
              </p>
            ) : !passTimings || passTimings.passes.length === 0 ? (
              <p className="text-[10px] italic text-muted-foreground">Waiting for timings…</p>
            ) : (
              // Stable execution order (the frame timeline) — never re-sort by magnitude,
              // which makes near-equal rows jump every poll. The bars show the cost driver.
              passTimings.passes.map((p) => {
                const share = budgetMs > 0 ? p.gpuMs / budgetMs : 0;
                const status = passStatus(p.gpuMs, budgetMs);
                return (
                  <div key={p.name} className="flex flex-col gap-0.5">
                    <div className="flex items-baseline justify-between gap-2">
                      <span className="truncate text-[11px] text-muted-foreground">{p.name}</span>
                      <span
                        className={cn(
                          "shrink-0 font-mono text-[11px] tabular-nums",
                          STATUS_TEXT[status],
                        )}
                      >
                        {p.gpuMs.toFixed(2)} ms · {(share * 100).toFixed(0)}%
                      </span>
                    </div>
                    <Bar fraction={share} status={status} />
                  </div>
                );
              })
            )}
          </section>

          {/* VRAM gauge (device-local heaps; known only while profiling). */}
          {vramKnown ? (
            <section className="flex flex-col gap-1 rounded-md border border-border bg-background px-2.5 py-2">
              <div className="flex items-baseline justify-between gap-2">
                <span className="text-[11px] font-medium text-foreground">VRAM</span>
                <span className={cn("font-mono text-[11px] tabular-nums", STATUS_TEXT[vramStat])}>
                  {formatMb(stats.vramUsageBytes)} / {formatMb(stats.vramBudgetBytes)} (
                  {(vramFrac * 100).toFixed(0)}%)
                </span>
              </div>
              <Bar fraction={vramFrac} status={vramStat} />
            </section>
          ) : null}

          {/* Alarm log: the recent FIRING/RESOLVED history. */}
          <section className="flex flex-col gap-1 rounded-md border border-border bg-background px-2.5 py-2">
            <span className="text-[11px] font-medium text-foreground">Alarms</span>
            {recentAlarms.length === 0 ? (
              <p className="text-[10px] italic text-muted-foreground">No alarms.</p>
            ) : (
              recentAlarms.map((event) => (
                <div
                  key={`${event.seq}`}
                  className="flex items-center justify-between gap-2 text-[10px]"
                >
                  <span className="flex min-w-0 items-center gap-1.5">
                    <span
                      className={cn("size-1.5 shrink-0 rounded-full", SEVERITY_DOT[event.severity])}
                    />
                    <span className="truncate text-muted-foreground">
                      {event.metric}
                      {event.pass ? ` · ${event.pass}` : ""}
                    </span>
                  </span>
                  <span className="shrink-0 font-mono tabular-nums text-muted-foreground">
                    {event.state === "resolved" ? "resolved" : event.value.toFixed(1)}
                  </span>
                </div>
              ))
            )}
          </section>

          <Separator className="my-0.5" />

          {/* Counters + active feature flags. */}
          <section className="flex flex-col gap-1 rounded-md border border-border bg-background px-2.5 py-2">
            <Stat label="Draw calls" value={stats.drawCalls} />
            <Stat label="Batches" value={stats.batches} />
            <Stat label="Instances" value={stats.instances} />
            <Stat label="Triangles" value={stats.triangles.toLocaleString()} />
            <Stat label="Descriptor binds" value={stats.descriptorBinds} />
            <Stat label="Pipelines" value={stats.pipelines} />
            <Stat label="BLAS count" value={stats.blasCount} />
            <Separator className="my-1" />
            <Stat label="AA" value={stats.aa} />
            <Stat label="Exposure (EV)" value={stats.exposureEv.toFixed(2)} />
            <Stat label="RT supported" value={stats.rtSupported ? "yes" : "no"} />
            <Separator className="my-1" />
            <Stat
              label="Engine frame"
              value={`${readout.fps.toFixed(0)} fps / ${readout.frameMs.toFixed(2)} ms`}
            />
            <Stat label="GPU" value={readout.gpuMs > 0 ? `${readout.gpuMs.toFixed(2)} ms` : "—"} />
            <Stat
              label="UI poll"
              value={readout.pollRateHz > 0 ? `${readout.pollRateHz.toFixed(1)} Hz` : "—"}
            />
            <Stat
              label="UI frame"
              value={
                readout.uiFrameRateHz > 0
                  ? `${readout.uiFrameRateHz.toFixed(0)} fps / ${readout.uiFrameMs.toFixed(1)} ms`
                  : "—"
              }
            />
          </section>

          <div className="grid grid-cols-[1fr_auto] items-center gap-1.5">
            <Label className="truncate text-[11px] font-normal text-muted-foreground">
              Anti-aliasing
            </Label>
            <Select value={stats.aa} disabled={!ready} onValueChange={(v) => onAa(v as AaMode)}>
              <SelectTrigger size="sm" className="h-7 w-[112px] font-mono text-[11px]">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {AA_MODES.map((m) => (
                  <SelectItem key={m.value} value={m.value} className="text-[11px]">
                    {m.label}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </div>

          {TOGGLES.map((t) => {
            const disabled = !ready || (t.rtGated === true && !stats.rtSupported);
            const tooltip =
              t.rtGated === true && !stats.rtSupported
                ? "Ray tracing not supported on this device"
                : undefined;
            return (
              <ToggleRow
                key={t.field}
                label={t.label}
                checked={stats[t.field] === true}
                disabled={disabled}
                tooltip={tooltip}
                onCheckedChange={(next) => onToggle(t.field, t.set, next)}
              />
            );
          })}

          <div className="grid grid-cols-[1fr_120px] items-center gap-1.5">
            <Label className="truncate text-[11px] font-normal text-muted-foreground">
              Exposure (EV)
            </Label>
            <NumberDrag
              value={stats.exposureEv}
              min={-8}
              max={8}
              step={0.05}
              onChange={onExposure}
              onDragStart={() => setDragActive(true)}
              onDragEnd={() => setDragActive(false)}
            />
          </div>

          {error ? (
            <p className="rounded-sm border border-destructive/40 bg-destructive/10 px-2 py-1 text-[11px] text-destructive">
              {error}
            </p>
          ) : null}
        </div>
      </ScrollArea>
    </div>
  );
}

function LegendDot({ color, label }: { color: string; label: string }) {
  return (
    <span className="flex items-center gap-1">
      <span className="size-1.5 rounded-full" style={{ backgroundColor: color }} />
      {label}
    </span>
  );
}

/// Normalize a rejected control call into a readable message. The Rust passthrough
/// rejects with the engine's error string (e.g. "ray tracing not supported …").
function errorText(err: unknown): string {
  if (typeof err === "string") {
    return err;
  }
  if (err instanceof Error) {
    return err.message;
  }
  return String(err);
}
