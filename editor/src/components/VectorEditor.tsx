/// Multi-axis vector editor (ported from the worktree `VectorEditor`, generalized
/// from a fixed Vec3 to N named axes). Each axis label is a pointer-capture
/// drag-scrub handle (clientX delta * step); each value is a numeric `<input>` that
/// swallows its own pointer so typing never starts a scrub. Used for `vec3`/`vec4`
/// fields. Unit conversion (degrees) is handled by `fieldRenderer` before/after, so
/// this widget stays unit-agnostic. Dumb: value + onChange per axis; the panel owns
/// coalescing and drag-gating.
import { useRef } from "react";
import { formatNumber } from "./NumberDrag";
import { Input } from "@/components/ui/input";

export interface VectorEditorProps {
  /// Ordered axis keys into the value record, e.g. ["x","y","z"] or ["x","y","z","w"].
  axes: readonly string[];
  value: Record<string, number>;
  step?: number;
  onChange(axis: string, value: number): void;
  onDragStart?(): void;
  onDragEnd?(): void;
}

export function VectorEditor({
  axes,
  value,
  step = 0.05,
  onChange,
  onDragStart,
  onDragEnd,
}: VectorEditorProps) {
  const dragRef = useRef<{ axis: string; startX: number; startValue: number } | null>(null);

  function beginDrag(axis: string, event: React.PointerEvent<HTMLLabelElement>): void {
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    const current = value[axis];
    dragRef.current = {
      axis,
      startX: event.clientX,
      startValue: Number.isFinite(current) ? current : 0,
    };
    onDragStart?.();
  }

  function updateDrag(event: React.PointerEvent<HTMLLabelElement>): void {
    const drag = dragRef.current;
    if (!drag) {
      return;
    }
    const delta = event.clientX - drag.startX;
    const next = drag.startValue + delta * step;
    onChange(drag.axis, Number(next.toFixed(3)));
  }

  function endDrag(): void {
    if (dragRef.current) {
      dragRef.current = null;
      onDragEnd?.();
    }
  }

  return (
    <div className="flex gap-1">
      {axes.map((axis) => (
        <label
          key={axis}
          className="flex min-w-0 flex-1 cursor-ew-resize items-center rounded-sm border border-border bg-background"
          onPointerDown={(event) => beginDrag(axis, event)}
          onPointerMove={updateDrag}
          onPointerUp={endDrag}
          onPointerCancel={endDrag}
        >
          <span className="select-none px-1 text-[10px] font-semibold text-muted-foreground">
            {axis.toUpperCase()}
          </span>
          <Input
            type="number"
            step={step}
            value={formatNumber(value[axis] ?? 0)}
            className="h-7 rounded-none border-0 bg-transparent px-1 py-0.5 font-mono text-[11px] shadow-none focus-visible:ring-0"
            onPointerDown={(event) => event.stopPropagation()}
            onChange={(event) => onChange(axis, Number(event.currentTarget.value))}
          />
        </label>
      ))}
    </div>
  );
}
