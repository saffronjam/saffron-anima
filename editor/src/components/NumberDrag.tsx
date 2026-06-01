/// A single-axis drag-scrub number field (ported from the worktree `VectorEditor`
/// drag math, generalized to one axis). Pointer-capture on the wrapper scrubs the
/// value by `clientX` delta * step; the numeric `<input>` swallows its own pointer
/// so typing in the box never starts a scrub. Optional `track` renders a 0..1-style
/// slider fill behind the value for `slider` fields. Dumb: value + onChange only;
/// the panel owns coalescing and drag-gating (onDragStart/onDragEnd bracket a scrub).
import { useRef } from "react";
import { Input } from "@/components/ui/input";
import { cn } from "@/lib/utils";

export function formatNumber(value: number): string {
  if (!Number.isFinite(value)) {
    return "0";
  }
  return Number(value.toFixed(3)).toString();
}

function clamp(value: number, min?: number, max?: number): number {
  let v = value;
  if (min !== undefined && v < min) {
    v = min;
  }
  if (max !== undefined && v > max) {
    v = max;
  }
  return v;
}

export interface NumberDragProps {
  value: number;
  step?: number;
  min?: number;
  max?: number;
  /// When true, draw a slider fill behind the value (used for `slider` fields).
  track?: boolean;
  onChange(value: number): void;
  /// Bracket a scrub gesture so the panel can gate the reconcile poll off.
  onDragStart?(): void;
  onDragEnd?(): void;
}

export function NumberDrag({
  value,
  step = 0.05,
  min,
  max,
  track = false,
  onChange,
  onDragStart,
  onDragEnd,
}: NumberDragProps) {
  const dragRef = useRef<{ startX: number; startValue: number } | null>(null);

  function beginDrag(event: React.PointerEvent<HTMLDivElement>): void {
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    dragRef.current = {
      startX: event.clientX,
      startValue: Number.isFinite(value) ? value : 0,
    };
    onDragStart?.();
  }

  function updateDrag(event: React.PointerEvent<HTMLDivElement>): void {
    const drag = dragRef.current;
    if (!drag) {
      return;
    }
    const delta = event.clientX - drag.startX;
    const next = clamp(drag.startValue + delta * step, min, max);
    onChange(Number(next.toFixed(3)));
  }

  function endDrag(): void {
    if (dragRef.current) {
      dragRef.current = null;
      onDragEnd?.();
    }
  }

  const fill =
    track && min !== undefined && max !== undefined && max > min
      ? ((clamp(value, min, max) - min) / (max - min)) * 100
      : null;

  return (
    <div
      className="relative cursor-ew-resize overflow-hidden rounded-sm"
      onPointerDown={beginDrag}
      onPointerMove={updateDrag}
      onPointerUp={endDrag}
      onPointerCancel={endDrag}
    >
      {fill !== null ? (
        <div
          className="pointer-events-none absolute inset-y-0 left-0 bg-primary/25"
          style={{ width: `${fill}%` }}
        />
      ) : null}
      <Input
        type="number"
        step={step}
        min={min}
        max={max}
        value={formatNumber(value)}
        className={cn(
          "relative h-7 rounded-sm bg-background px-1.5 py-0.5 font-mono text-[11px]",
          track && "bg-transparent",
        )}
        onPointerDown={(event) => event.stopPropagation()}
        onChange={(event) => onChange(clamp(Number(event.currentTarget.value), min, max))}
      />
    </div>
  );
}
