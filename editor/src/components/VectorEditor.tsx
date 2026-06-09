/// Multi-axis vector editor (ported from the worktree `VectorEditor`, generalized
/// from a fixed Vec3 to N named axes). Each axis label is a pointer-capture
/// drag-scrub handle (clientX delta * step); each value is a numeric `<input>` that
/// swallows its own pointer so typing never starts a scrub. Used for `vec3`/`vec4`
/// fields. Unit conversion (degrees) is handled by `fieldRenderer` before/after, so
/// this widget stays unit-agnostic. Renders drag-local state (useScrubValue) so the
/// readouts never wait on the wire; emits one atomic patch per edit. The panel owns
/// coalescing and drag-gating.
import { useRef } from "react";
import { formatNumber } from "./NumberDrag";
import { Input } from "@/components/ui/input";
import { cn } from "@/lib/utils";
import { useScrubValue } from "@/lib/useScrubValue";

/// Axis label colors matching the viewport gizmo (X red, Y green, Z blue): a dark
/// tint behind a bright label.
const AXIS_COLORS: Record<string, string> = {
  x: "bg-red-950 text-red-400",
  y: "bg-green-950 text-green-400",
  z: "bg-blue-950 text-blue-400",
};

export interface VectorEditorProps {
  /// Ordered axis keys into the value record, e.g. ["x","y","z"] or ["x","y","z","w"].
  axes: readonly string[];
  value: Record<string, number>;
  step?: number;
  /// One atomic patch per edit (the changed axes), never per-axis calls racing
  /// onto a stale base.
  onChange(patch: Record<string, number>): void;
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
  const scrub = useScrubValue(value, onChange);
  const dragRef = useRef<{ axis: string; startX: number; startValue: number } | null>(null);

  function beginDrag(axis: string, event: React.PointerEvent<HTMLLabelElement>): void {
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    const current = scrub.value[axis];
    dragRef.current = {
      axis,
      startX: event.clientX,
      startValue: Number.isFinite(current) ? current : 0,
    };
    scrub.begin();
    onDragStart?.();
  }

  function updateDrag(event: React.PointerEvent<HTMLLabelElement>): void {
    const drag = dragRef.current;
    if (!drag) {
      return;
    }
    const delta = event.clientX - drag.startX;
    const next = drag.startValue + delta * step;
    scrub.set({ ...scrub.value, [drag.axis]: Number(next.toFixed(3)) });
  }

  function endDrag(): void {
    if (dragRef.current) {
      dragRef.current = null;
      scrub.end();
      onDragEnd?.();
    }
  }

  return (
    <div className="flex gap-1">
      {axes.map((axis) => (
        <label
          key={axis}
          className="flex min-w-0 flex-1 cursor-ew-resize items-center overflow-hidden rounded-sm border border-border bg-background"
          onPointerDown={(event) => beginDrag(axis, event)}
          onPointerMove={updateDrag}
          onPointerUp={endDrag}
          onPointerCancel={endDrag}
        >
          <span
            className={cn(
              "flex items-center self-stretch px-1 text-[10px] font-semibold select-none",
              AXIS_COLORS[axis] ?? "bg-muted text-muted-foreground",
            )}
          >
            {axis.toUpperCase()}
          </span>
          <Input
            type="number"
            step={step}
            value={formatNumber(scrub.value[axis] ?? 0)}
            className="h-7 rounded-none border-0 bg-transparent px-1 py-0.5 font-mono text-[11px] shadow-none focus-visible:ring-0"
            onPointerDown={(event) => event.stopPropagation()}
            onChange={(event) =>
              scrub.set({ ...scrub.value, [axis]: Number(event.currentTarget.value) })
            }
          />
        </label>
      ))}
    </div>
  );
}
