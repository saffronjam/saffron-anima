/// The Render panel: the project's render configuration — anti-aliasing, the feature
/// toggles, and tonemap exposure. These persist with the project (the engine serializes
/// them into the `renderSettings` block and reapplies them on load), so they sit beside
/// Environment as scene-presentation config, not in the Stats telemetry tool.
///
/// Values are read with a shallow-selected subset of `renderStats` so the panel only
/// re-renders when a config field actually changes — not on the 20 Hz render-stats poll
/// that rewrites the full bag. A write optimistically folds the new value in (and the
/// echoed result) so the control reflects the change at once; the reconcile poll re-reads
/// the full bag right after.
import { useShallow } from "zustand/react/shallow";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { NumberDrag } from "../components/NumberDrag";
import { errorText, notifyError } from "../lib/flash";
import type { RenderStats } from "../protocol";
import { Label } from "@/components/ui/label";
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

export function RenderPanel() {
  const ready = useEditorStore((s) => s.engineStatus.phase === "ready");
  const hasStats = useEditorStore((s) => s.renderStats !== null);
  const setRenderStats = useEditorStore((s) => s.setRenderStats);
  const setDragActive = useEditorStore((s) => s.setDragActive);
  const cfg = useEditorStore(
    useShallow((s) => {
      const r = s.renderStats;
      return {
        aa: (r?.aa ?? "off") as AaMode,
        exposureEv: r?.exposureEv ?? 0,
        rtSupported: r?.rtSupported ?? false,
        clustered: r?.clustered ?? false,
        depthPrepass: r?.depthPrepass ?? false,
        shadows: r?.shadows ?? false,
        ibl: r?.ibl ?? false,
        ssao: r?.ssao ?? false,
        contactShadows: r?.contactShadows ?? false,
        ssgi: r?.ssgi ?? false,
        ddgi: r?.ddgi ?? false,
        rtShadows: r?.rtShadows ?? false,
        restir: r?.restir ?? false,
      };
    }),
  );

  const optimistic = (patch: Partial<RenderStats>): void => {
    const cur = useEditorStore.getState().renderStats;
    if (cur) {
      setRenderStats({ ...cur, ...patch });
    }
  };

  const onAa = (mode: AaMode): void => {
    optimistic({ aa: mode });
    void client
      .setAa(mode)
      .then((res) => optimistic({ aa: res.aa }))
      .catch((err: unknown) => notifyError(errorText(err)));
  };

  const onToggle = (
    field: keyof RenderStats,
    set: (on: boolean) => Promise<unknown>,
    next: boolean,
  ): void => {
    const cur = useEditorStore.getState().renderStats;
    const previous = cur ? cur[field] : next;
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
        notifyError(errorText(err));
      });
  };

  const onExposure = (ev: number): void => {
    optimistic({ exposureEv: ev });
    void client
      .setExposure(ev)
      .then((res) => optimistic({ exposureEv: res.exposureEv }))
      .catch((err: unknown) => notifyError(errorText(err)));
  };

  if (!hasStats) {
    return (
      <div className="flex h-full min-h-0 flex-col">
        <div className="p-3.5 text-center italic text-muted-foreground">
          {ready ? "Waiting for stats…" : "Engine not ready"}
        </div>
      </div>
    );
  }

  return (
    <div className="flex h-full min-h-0 flex-col">
      <ScrollArea className="min-h-0 flex-1">
        <div className="flex flex-col gap-2 p-2.5">
          <div className="grid grid-cols-[1fr_auto] items-center gap-1.5">
            <Label className="truncate text-[11px] font-normal text-muted-foreground">
              Anti-aliasing
            </Label>
            <Select value={cfg.aa} disabled={!ready} onValueChange={(v) => onAa(v as AaMode)}>
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
            const disabled = !ready || (t.rtGated === true && !cfg.rtSupported);
            const tooltip =
              t.rtGated === true && !cfg.rtSupported
                ? "Ray tracing not supported on this device"
                : undefined;
            return (
              <ToggleRow
                key={t.field}
                label={t.label}
                checked={cfg[t.field as keyof typeof cfg] === true}
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
              value={cfg.exposureEv}
              min={-8}
              max={8}
              step={0.05}
              onChange={onExposure}
              onDragStart={() => setDragActive(true)}
              onDragEnd={() => setDragActive(false)}
            />
          </div>
        </div>
      </ScrollArea>
    </div>
  );
}
