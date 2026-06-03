/// The Environment panel: the React port of the C++ `environmentPanel`
/// (editor_panels.cpp:191-224), bound to `get-environment` / `set-environment`.
/// Sky Mode (Color/Texture/Procedural) gates the relevant fields; clearColor and
/// ambientColor use ColorField; skyIntensity/ambientIntensity use NumberDrag; the
/// sky texture uses the phase-7 AssetPicker (texture catalog).
///
/// Units (the 57x bug guard): skyRotation is RADIANS on the wire but shown in
/// DEGREES in the UI — conversion happens ONLY at the rotation widget boundary
/// here. Exposure is deliberately NOT here: `SceneEnvironment.exposure` is reserved
/// on the wire; the effective tonemap exposure is the render-side `set-exposure`,
/// surfaced in the Render Stats panel.
///
/// `set-environment` is a server-side MERGE over the current environment, so every
/// write sends only the one named field that changed (a `Partial<Environment>`).
/// High-frequency edits (drags/sliders) funnel through per-field coalescers and the
/// drag bracket flips `store.dragActive` so the reconcile poll won't clobber the
/// optimistic value mid-scrub.
import { useEffect, useMemo, useRef } from "react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { makeCoalescer, type Coalescer } from "../control/coalesce";
import { NumberDrag } from "../components/NumberDrag";
import { ColorField } from "../components/ColorField";
import { AssetPicker } from "../components/AssetPicker";
import type { Environment, Vec3 } from "../protocol";
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

const RAD_TO_DEG = 180 / Math.PI;
const DEG_TO_RAD = Math.PI / 180;

type SkyMode = Environment["skyMode"];

const SKY_MODES: { value: SkyMode; label: string }[] = [
  { value: "color", label: "Color" },
  { value: "texture", label: "Texture" },
  { value: "procedural", label: "Procedural" },
];

/// A labelled row: a left caption + the widget, matching the inspector's grid.
function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="grid grid-cols-[96px_1fr] items-center gap-1.5">
      <Label className="truncate text-[11px] font-normal text-muted-foreground">{label}</Label>
      <div className="min-w-0">{children}</div>
    </div>
  );
}

export function EnvironmentPanel() {
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const sceneVersion = useEditorStore((s) => s.sceneVersion);
  const environment = useEditorStore((s) => s.environment);
  const setEnvironment = useEditorStore((s) => s.setEnvironment);
  const setDragActive = useEditorStore((s) => s.setDragActive);

  const ready = phase === "ready";

  // Fetch on mount and whenever the scene/project changes (a load swaps the env).
  // The reconcile poll also refreshes it on a scene change; this guarantees the
  // panel is correct even if the poll's gate (focus/drag) skipped that tick.
  useEffect(() => {
    if (!ready) {
      return;
    }
    let cancelled = false;
    void client
      .getEnvironment()
      .then((env) => {
        if (!cancelled && !useEditorStore.getState().dragActive) {
          useEditorStore.getState().setEnvironment(env);
        }
      })
      .catch(() => {});
    return () => {
      cancelled = true;
    };
  }, [ready, sceneVersion]);

  // Per-field coalescers, rebuilt when the field set is stable. The send pushes the
  // single named field through `set-environment` (server merges) and folds the
  // merged result back into the store so a clamp/normalize round-trips.
  const coalescers = useRef(new Map<keyof Environment, Coalescer<Partial<Environment>>>());
  const coalescerFor = useMemo(
    () =>
      (field: keyof Environment): Coalescer<Partial<Environment>> => {
        let c = coalescers.current.get(field);
        if (!c) {
          c = makeCoalescer<Partial<Environment>>({
            send: async (patch) => {
              const merged = await client.setEnvironment(patch);
              if (!useEditorStore.getState().dragActive) {
                useEditorStore.getState().setEnvironment(merged);
              }
            },
          });
          coalescers.current.set(field, c);
        }
        return c;
      },
    [],
  );

  if (!environment) {
    return (
      <div className="flex h-full min-h-0 flex-col">
        <PanelHeader />
        <div className="p-3.5 text-center italic text-muted-foreground">
          {ready ? "Loading environment…" : "Engine not ready"}
        </div>
      </div>
    );
  }

  const env = environment;

  // Optimistic local write + coalesced send of the one changed field.
  const patch = (field: keyof Environment, value: Environment[keyof Environment]): void => {
    setEnvironment({ ...env, [field]: value } as Environment);
    coalescerFor(field).push({ [field]: value } as Partial<Environment>);
  };

  const onDragStart = (): void => setDragActive(true);
  const onDragEnd = (): void => setDragActive(false);

  const onVecChannel =
    (field: "clearColor" | "ambientColor") =>
    (axis: string, value: number): void => {
      const next = { ...(env[field] as Vec3), [axis]: value } as Vec3;
      patch(field, next);
    };

  return (
    <div className="flex h-full min-h-0 flex-col">
      <PanelHeader />
      <ScrollArea className="min-h-0 flex-1">
        <div className="flex flex-col gap-2 p-2.5">
          <Row label="Sky Mode">
            <Select
              value={env.skyMode}
              onValueChange={(value) => patch("skyMode", value as SkyMode)}
            >
              <SelectTrigger size="sm" className="h-7 w-full font-mono text-[11px]">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {SKY_MODES.map((m) => (
                  <SelectItem key={m.value} value={m.value} className="text-[11px]">
                    {m.label}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </Row>

          {env.skyMode === "color" ? (
            <Row label="Clear Color">
              <ColorField
                kind="color3"
                value={env.clearColor as unknown as Record<string, number>}
                onChange={onVecChannel("clearColor")}
                onDragStart={onDragStart}
                onDragEnd={onDragEnd}
              />
            </Row>
          ) : null}

          {env.skyMode === "texture" ? (
            <Row label="Sky Texture">
              <AssetPicker
                value={env.skyTexture}
                assetType="texture"
                onChange={(id) => patch("skyTexture", id)}
              />
            </Row>
          ) : null}

          <Row label="Intensity">
            <NumberDrag
              value={env.skyIntensity}
              min={0}
              max={100}
              step={0.01}
              onChange={(v) => patch("skyIntensity", v)}
              onDragStart={onDragStart}
              onDragEnd={onDragEnd}
            />
          </Row>

          <Row label="Rotation (°)">
            <NumberDrag
              value={env.skyRotation * RAD_TO_DEG}
              min={-360}
              max={360}
              step={0.5}
              onChange={(deg) => patch("skyRotation", deg * DEG_TO_RAD)}
              onDragStart={onDragStart}
              onDragEnd={onDragEnd}
            />
          </Row>

          <Row label="Visible">
            <Switch
              checked={env.visible}
              onCheckedChange={(checked) => patch("visible", checked)}
            />
          </Row>

          <Separator className="my-1" />

          <Row label="Sky Ambient">
            <Switch
              checked={env.useSkyForAmbient}
              onCheckedChange={(checked) => patch("useSkyForAmbient", checked)}
            />
          </Row>

          <Row label="Ambient Color">
            <ColorField
              kind="color3"
              value={env.ambientColor as unknown as Record<string, number>}
              onChange={onVecChannel("ambientColor")}
              onDragStart={onDragStart}
              onDragEnd={onDragEnd}
            />
          </Row>

          <Row label="Ambient Int.">
            <NumberDrag
              value={env.ambientIntensity}
              min={0}
              max={10}
              step={0.005}
              onChange={(v) => patch("ambientIntensity", v)}
              onDragStart={onDragStart}
              onDragEnd={onDragEnd}
            />
          </Row>
        </div>
      </ScrollArea>
    </div>
  );
}

function PanelHeader() {
  return (
    <div className="flex h-10 flex-none items-center border-b border-border px-3">
      <span className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">
        Environment
      </span>
    </div>
  );
}
