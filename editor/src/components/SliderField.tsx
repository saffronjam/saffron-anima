/// Bounded scalar field rendered as a shadcn Slider + a monospace numeric readout
/// (used for `slider`-kind fields like Material.metallic/roughness, clamped 0..1).
/// Dumb: value + onChange; the panel owns coalescing and drag-gating. The Radix
/// Slider's `onValueChange` is the live scrub (drag) and `onValueCommit` is the end,
/// so they bracket onDragStart/onDragEnd exactly like NumberDrag's pointer scrub.
import { Slider } from "@/components/ui/slider";

export interface SliderFieldProps {
  value: number;
  min?: number;
  max?: number;
  step?: number;
  onChange(value: number): void;
  /// Bracket a scrub gesture so the panel can gate the reconcile poll off.
  onDragStart?(): void;
  onDragEnd?(): void;
}

function clamp(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) {
    return min;
  }
  return Math.min(max, Math.max(min, value));
}

export function SliderField({
  value,
  min = 0,
  max = 1,
  step = 0.01,
  onChange,
  onDragStart,
  onDragEnd,
}: SliderFieldProps) {
  const current = clamp(value, min, max);
  return (
    <div className="flex items-center gap-2">
      <Slider
        className="min-w-0 flex-1"
        value={[current]}
        min={min}
        max={max}
        step={step}
        onPointerDown={() => onDragStart?.()}
        onValueChange={(values) => onChange(Number(clamp(values[0] ?? current, min, max).toFixed(3)))}
        onValueCommit={() => onDragEnd?.()}
      />
      <span className="w-9 flex-none text-right font-mono text-[11px] tabular-nums text-muted-foreground">
        {current.toFixed(2)}
      </span>
    </div>
  );
}
