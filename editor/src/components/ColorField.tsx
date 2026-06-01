/// Color field for Vec3 (color3) / Vec4 (color4) float channels. Channels are
/// LINEAR floats in 0..1 on the wire (matching the C++ `ColorEdit3`/`ColorEdit4`
/// linear behavior the inspector ports). The swatch is an `<input type="color">`
/// (hex, 8-bit) for the RGB picker; per-channel numeric inputs keep HDR-range and
/// alpha editable beyond the 8-bit swatch. Dumb: value + onChange; the panel owns
/// coalescing and drag-gating.
import { formatNumber } from "./NumberDrag";
import { Input } from "@/components/ui/input";

export interface ColorFieldProps {
  /// "color3" for Vec3 (rgb) or "color4" for Vec4 (rgba); alpha shown only for color4.
  kind: "color3" | "color4";
  value: Record<string, number>;
  onChange(axis: string, value: number): void;
  onDragStart?(): void;
  onDragEnd?(): void;
}

function channelToHex(c: number): string {
  const v = Math.round(Math.min(1, Math.max(0, Number.isFinite(c) ? c : 0)) * 255);
  return v.toString(16).padStart(2, "0");
}

function hexToChannel(hex: string): number {
  const v = parseInt(hex, 16);
  return Number.isFinite(v) ? v / 255 : 0;
}

export function ColorField({ kind, value, onChange, onDragStart, onDragEnd }: ColorFieldProps) {
  const hasAlpha = kind === "color4";
  const channels = hasAlpha ? (["x", "y", "z", "w"] as const) : (["x", "y", "z"] as const);
  const labels: Record<string, string> = { x: "R", y: "G", z: "B", w: "A" };

  const hex = `#${channelToHex(value.x ?? 0)}${channelToHex(value.y ?? 0)}${channelToHex(value.z ?? 0)}`;

  function onSwatch(next: string): void {
    onDragStart?.();
    onChange("x", Number(hexToChannel(next.slice(1, 3)).toFixed(3)));
    onChange("y", Number(hexToChannel(next.slice(3, 5)).toFixed(3)));
    onChange("z", Number(hexToChannel(next.slice(5, 7)).toFixed(3)));
    onDragEnd?.();
  }

  return (
    <div className="flex items-center gap-1.5">
      <input
        type="color"
        className="h-[22px] w-[26px] flex-none cursor-pointer rounded-sm border border-border bg-transparent p-0"
        value={hex}
        onChange={(event) => onSwatch(event.currentTarget.value)}
      />
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
