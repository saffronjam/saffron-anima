/// Color field for Vec3 (color3) / Vec4 (color4) float channels. Channels are
/// LINEAR floats in 0..1 on the wire (matching the C++ `ColorEdit3`/`ColorEdit4`
/// linear behavior the inspector ports). The swatch opens a Popover with a
/// saturation/hue (and alpha) canvas; per-channel numeric inputs keep HDR-range and
/// alpha editable beyond the 0..1 the canvas exposes. Dumb: value + onChange; the
/// panel owns coalescing and drag-gating.
import { RgbaColorPicker, RgbColorPicker } from "react-colorful";
import { formatNumber } from "./NumberDrag";
import { Input } from "@/components/ui/input";
import { Popover, PopoverContent, PopoverTrigger } from "@/components/ui/popover";
import { cn } from "@/lib/utils";

export interface ColorFieldProps {
  /// "color3" for Vec3 (rgb) or "color4" for Vec4 (rgba); alpha shown only for color4.
  kind: "color3" | "color4";
  value: Record<string, number>;
  onChange(axis: string, value: number): void;
  onDragStart?(): void;
  onDragEnd?(): void;
}

function channelToByte(c: number): number {
  return Math.round(Math.min(1, Math.max(0, Number.isFinite(c) ? c : 0)) * 255);
}

function channelToHex(c: number): string {
  return channelToByte(c).toString(16).padStart(2, "0");
}

export function ColorField({ kind, value, onChange, onDragStart, onDragEnd }: ColorFieldProps) {
  const hasAlpha = kind === "color4";
  const channels = hasAlpha ? (["x", "y", "z", "w"] as const) : (["x", "y", "z"] as const);
  const labels: Record<string, string> = { x: "R", y: "G", z: "B", w: "A" };

  const hex = `#${channelToHex(value.x ?? 0)}${channelToHex(value.y ?? 0)}${channelToHex(value.z ?? 0)}`;

  // Gate the reconcile poll across a canvas drag: a pointerdown on the picker opens
  // the drag, a single window pointerup closes it.
  const beginCanvasDrag = (): void => {
    onDragStart?.();
    const end = (): void => {
      onDragEnd?.();
      window.removeEventListener("pointerup", end);
    };
    window.addEventListener("pointerup", end);
  };

  const setRgb = (rgb: { r: number; g: number; b: number }): void => {
    onChange("x", Number((rgb.r / 255).toFixed(3)));
    onChange("y", Number((rgb.g / 255).toFixed(3)));
    onChange("z", Number((rgb.b / 255).toFixed(3)));
  };

  return (
    <div className="flex items-center gap-1.5">
      <Popover>
        <PopoverTrigger asChild>
          <button
            type="button"
            className="h-[22px] w-[26px] flex-none cursor-pointer rounded-sm border border-border"
            style={{ backgroundColor: hex }}
            aria-label="Pick color"
          />
        </PopoverTrigger>
        <PopoverContent className="w-auto p-2" align="start" onPointerDownCapture={beginCanvasDrag}>
          <div className={cn("color-picker-canvas", hasAlpha && "color-picker-canvas--alpha")}>
            {hasAlpha ? (
              <RgbaColorPicker
                color={{
                  r: channelToByte(value.x ?? 0),
                  g: channelToByte(value.y ?? 0),
                  b: channelToByte(value.z ?? 0),
                  a: Math.min(1, Math.max(0, value.w ?? 1)),
                }}
                onChange={(c) => {
                  setRgb(c);
                  onChange("w", Number(c.a.toFixed(3)));
                }}
              />
            ) : (
              <RgbColorPicker
                color={{
                  r: channelToByte(value.x ?? 0),
                  g: channelToByte(value.y ?? 0),
                  b: channelToByte(value.z ?? 0),
                }}
                onChange={setRgb}
              />
            )}
          </div>
        </PopoverContent>
      </Popover>
      <div className="flex min-w-0 flex-1 gap-0.5">
        {channels.map((axis) => (
          <label
            key={axis}
            className="flex min-w-0 flex-1 items-center rounded-sm border border-border bg-background"
          >
            <span className="px-0.5 text-[9px] font-semibold text-muted-foreground">
              {labels[axis]}
            </span>
            <Input
              type="number"
              step={0.01}
              min={0}
              max={axis === "w" ? 1 : undefined}
              value={formatNumber(value[axis] ?? 0)}
              className="h-7 rounded-none border-0 bg-transparent px-0.5 py-0.5 font-mono text-[11px] shadow-none focus-visible:ring-0"
              onChange={(event) => onChange(axis, Number(event.currentTarget.value))}
            />
          </label>
        ))}
      </div>
    </div>
  );
}
