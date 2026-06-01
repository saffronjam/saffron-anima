/// The Render Stats panel: a live readout of `render-stats` (counters + active
/// feature toggles) plus the render toggles that drive them. Mirrors the C++
/// render-stats / toggle surface, with one important honesty caveat:
///
///   - There is NO engine frame rate on the control wire. The "UI poll" rate is
///     the WEBVIEW reconcile cadence (a client-side EMA), NOT the renderer's fps —
///     the native viewport paints independently of this webview. It is labelled as
///     such, never as engine fps.
///
/// The RT-gated toggles (`rtShadows`, `restir`) are DISABLED when
/// `rtSupported === false`; even when enabled, a `set-*` may still reject with the
/// typed "ray tracing not supported" error, so those calls are wrapped and any
/// rejection is surfaced inline (no silent failure). Each toggle optimistically
/// updates `store.renderStats` from the echoed flag; the reconcile poll re-reads
/// the full bag on the next tick so derived counters (e.g. batches under MSAA) stay
/// accurate.
import { useState } from "react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { NumberDrag } from "../components/NumberDrag";
import type { RenderStats } from "../protocol";
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
import {
  Tooltip,
  TooltipContent,
  TooltipTrigger,
} from "@/components/ui/tooltip";

type AaMode = RenderStats["aa"];

const AA_MODES: { value: AaMode; label: string }[] = [
  { value: "off", label: "Off" },
  { value: "fxaa", label: "FXAA" },
  { value: "taa", label: "TAA" },
  { value: "msaa2", label: "MSAA 2x" },
  { value: "msaa4", label: "MSAA 4x" },
  { value: "msaa8", label: "MSAA 8x" },
];

/// The boolean feature toggles (label + the stat field + its setter). RT-gated
/// rows carry `rtGated` so the panel disables them when the device lacks support.
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

export function RenderStatsPanel() {
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const stats = useEditorStore((s) => s.renderStats);
  const pollRateHz = useEditorStore((s) => s.pollRateHz);
  const setRenderStats = useEditorStore((s) => s.setRenderStats);
  const setDragActive = useEditorStore((s) => s.setDragActive);
  const [error, setError] = useState<string | null>(null);

  const ready = phase === "ready";

  if (!stats) {
    return (
      <div className="flex h-full min-h-0 flex-col">
        <PanelHeader />
        <div className="p-3.5 text-center italic text-muted-foreground">
          {ready ? "Waiting for stats…" : "Engine not ready"}
        </div>
      </div>
    );
  }

  // Optimistically fold an echoed flag into the live stats; the next poll re-reads
  // the full bag so derived counters stay accurate.
  const optimistic = (patch: Partial<RenderStats>): void => {
    setRenderStats({ ...stats, ...patch });
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
        // The echo is `{ [field]: boolean }`; fold the authoritative value back.
        const echoed = (res as Record<string, unknown>)[field];
        if (typeof echoed === "boolean") {
          optimistic({ [field]: echoed } as Partial<RenderStats>);
        }
      })
      .catch((err: unknown) => {
        // Revert the optimistic state (e.g. an RT toggle the engine rejected).
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

  return (
    <div className="flex h-full min-h-0 flex-col">
      <PanelHeader />
      <ScrollArea className="min-h-0 flex-1">
        <div className="flex flex-col gap-2 p-2.5">
          <section className="flex flex-col gap-1 rounded-md border border-border bg-background px-2.5 py-2">
            <Stat label="Draw calls" value={stats.drawCalls} />
            <Stat label="Batches" value={stats.batches} />
            <Stat label="Instances" value={stats.instances} />
            <Stat label="Pipelines" value={stats.pipelines} />
            <Stat label="BLAS count" value={stats.blasCount} />
            <Separator className="my-1" />
            <Stat label="AA" value={stats.aa} />
            <Stat label="Exposure (EV)" value={stats.exposureEv.toFixed(2)} />
            <Stat label="HDR" value={stats.hdr ? "yes" : "no"} />
            <Stat label="RT supported" value={stats.rtSupported ? "yes" : "no"} />
            <Stat
              label="UI poll"
              value={pollRateHz > 0 ? `${pollRateHz.toFixed(1)} Hz` : "—"}
            />
          </section>

          <p className="px-0.5 text-[10px] italic leading-tight text-muted-foreground">
            UI poll is the webview reconcile rate, not the engine frame rate.
          </p>

          <Separator className="my-0.5" />

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

function PanelHeader() {
  return (
    <div className="flex h-[30px] flex-none items-center border-b border-border px-2.5">
      <span className="text-[11px] font-semibold uppercase tracking-wide text-muted-foreground">
        Render Stats
      </span>
    </div>
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
