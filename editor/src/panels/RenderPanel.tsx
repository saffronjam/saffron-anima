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
import { useEffect, useRef } from "react";
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
type ViewMode = RenderStats["viewMode"];

const AA_MODES: { value: AaMode; label: string }[] = [
  { value: "off", label: "Off" },
  { value: "fxaa", label: "FXAA" },
  { value: "taa", label: "TAA" },
  { value: "msaa2", label: "MSAA 2x" },
  { value: "msaa4", label: "MSAA 4x" },
  { value: "msaa8", label: "MSAA 8x" },
];

/// The debug render-output mode. Transient (not persisted, not undoable); only implemented
/// modes are listed, so the dropdown never offers a value the engine ignores.
const VIEW_MODES: { value: ViewMode; label: string }[] = [
  { value: "lit", label: "Lit" },
  { value: "wireframe", label: "Wireframe" },
  { value: "albedo", label: "Albedo" },
  { value: "normal", label: "Normal" },
  { value: "roughness", label: "Roughness" },
  { value: "metallic", label: "Metallic" },
  { value: "emissive", label: "Emissive" },
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

/// Debug-visualization overlays (set-debug-overlays). Persisted with the project but not undoable —
/// distinct from the feature toggles above, which are project render config.
const DEBUG_OVERLAYS: {
  label: string;
  field: "bounds" | "sceneAabb" | "lightVolumes" | "grid" | "colliders";
}[] = [
  { label: "Bounding Boxes", field: "bounds" },
  { label: "Scene AABB", field: "sceneAabb" },
  { label: "Light Volumes", field: "lightVolumes" },
  { label: "Grid", field: "grid" },
  { label: "Colliders", field: "colliders" },
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
  const debugOverlays = useEditorStore((s) => s.debugOverlays);
  const setDebugOverlays = useEditorStore((s) => s.setDebugOverlays);
  const cfg = useEditorStore(
    useShallow((s) => {
      const r = s.renderStats;
      return {
        aa: (r?.aa ?? "off") as AaMode,
        viewMode: (r?.viewMode ?? "lit") as ViewMode,
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

  // Debug overlays persist with the project but are not undoable (view state, not scene content).
  // Fetch once on mount; the render-panel-gated poll keeps them live (and reflects external `se`).
  useEffect(() => {
    if (ready && debugOverlays === null) {
      void client
        .getDebugOverlays()
        .then(setDebugOverlays)
        .catch(() => {
          // Engine briefly busy; the render-panel poll picks the overlays up on its next tick.
        });
    }
  }, [ready, debugOverlays, setDebugOverlays]);

  const onDebugToggle = (field: (typeof DEBUG_OVERLAYS)[number]["field"], next: boolean): void => {
    const previous = useEditorStore.getState().debugOverlays;
    if (previous) {
      setDebugOverlays({ ...previous, [field]: next });
    }
    void client
      .setDebugOverlays({ [field]: next })
      .then(setDebugOverlays)
      .catch((err: unknown) => {
        if (previous) {
          setDebugOverlays(previous);
        }
        notifyError(errorText(err));
      });
  };

  // Render settings persist with the project, so their edits are scene-tab undoable. A
  // toggle/AA records inline (discrete); exposure scrubbing records one entry per gesture.
  const recordRender = (
    label: string,
    undo: () => Promise<unknown>,
    redo: () => Promise<unknown>,
  ): void => {
    useEditorStore.getState().pushEdit({ label, undo, redo }, "scene");
  };

  const onAa = (mode: AaMode): void => {
    const prior = useEditorStore.getState().renderStats?.aa ?? "off";
    optimistic({ aa: mode });
    if (prior !== mode) {
      recordRender(
        "Anti-aliasing",
        () => client.setAa(prior),
        () => client.setAa(mode),
      );
    }
    void client
      .setAa(mode)
      .then((res) => optimistic({ aa: res.aa }))
      .catch((err: unknown) => notifyError(errorText(err)));
  };

  // View mode is a transient debug output, not project config — optimistic + echo, no undo record.
  const onViewMode = (mode: ViewMode): void => {
    optimistic({ viewMode: mode });
    void client
      .setViewMode(mode)
      .then((res) => optimistic({ viewMode: res.viewMode }))
      .catch((err: unknown) => notifyError(errorText(err)));
  };

  const onToggle = (
    field: keyof RenderStats,
    label: string,
    set: (on: boolean) => Promise<unknown>,
    next: boolean,
  ): void => {
    const cur = useEditorStore.getState().renderStats;
    const previous = cur ? cur[field] === true : !next;
    optimistic({ [field]: next } as Partial<RenderStats>);
    if (previous !== next) {
      recordRender(
        label,
        () => set(previous),
        () => set(next),
      );
    }
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

  // Exposure scrub: capture the prior at drag start, record once at drag end. A typed
  // edit (no gesture) records inline.
  const exposurePrior = useRef<number | null>(null);
  const onExposureDragStart = (): void => {
    exposurePrior.current = useEditorStore.getState().renderStats?.exposureEv ?? 0;
    setDragActive(true);
  };
  const onExposureDragEnd = (): void => {
    setDragActive(false);
    const prior = exposurePrior.current;
    exposurePrior.current = null;
    if (prior === null) {
      return;
    }
    const after = useEditorStore.getState().renderStats?.exposureEv ?? 0;
    if (prior !== after) {
      recordRender(
        "Exposure",
        () => client.setExposure(prior),
        () => client.setExposure(after),
      );
    }
  };
  const onExposure = (ev: number): void => {
    if (exposurePrior.current === null) {
      const prior = useEditorStore.getState().renderStats?.exposureEv ?? 0;
      if (prior !== ev) {
        recordRender(
          "Exposure",
          () => client.setExposure(prior),
          () => client.setExposure(ev),
        );
      }
    }
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

          <div className="grid grid-cols-[1fr_auto] items-center gap-1.5">
            <Label className="truncate text-[11px] font-normal text-muted-foreground">
              View Mode
            </Label>
            <Select
              value={cfg.viewMode}
              disabled={!ready}
              onValueChange={(v) => onViewMode(v as ViewMode)}
            >
              <SelectTrigger size="sm" className="h-7 w-[112px] font-mono text-[11px]">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {VIEW_MODES.map((m) => (
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
                onCheckedChange={(next) => onToggle(t.field, t.label, t.set, next)}
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
              onDragStart={onExposureDragStart}
              onDragEnd={onExposureDragEnd}
            />
          </div>

          <div className="mt-1 border-t border-border pt-2.5">
            <Label className="text-[10px] font-medium uppercase tracking-wide text-muted-foreground">
              Debug
            </Label>
          </div>
          {DEBUG_OVERLAYS.map((d) => (
            <ToggleRow
              key={d.field}
              label={d.label}
              checked={debugOverlays ? debugOverlays[d.field] : false}
              disabled={!ready}
              onCheckedChange={(next) => onDebugToggle(d.field, next)}
            />
          ))}
        </div>
      </ScrollArea>
    </div>
  );
}
