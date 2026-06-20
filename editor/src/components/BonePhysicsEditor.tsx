/// Per-bone ragdoll/collision editor for BonePhysics.bones — a fixed-length array positionally 1:1
/// with SkinnedMesh.bones (no add/remove/reorder; the skeleton owns the length). One collapsible
/// card per bone, labeled by joint name, with a name filter. Each card tunes the Jolt body the
/// physics build reads at the next Play: collider half-size, mass, the joint constraint to the
/// parent, its swing/twist limits (radians on the wire, shown in degrees), and the PD drive gains.
import { useState, type ReactNode } from "react";
import { ChevronDown, ChevronRight } from "lucide-react";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { NumberDrag } from "./NumberDrag";
import { VectorEditor } from "./VectorEditor";
import { EnumField } from "./EnumField";
import type { Joint } from "./BoneSelect";
import { DEG_TO_RAD, RAD_TO_DEG } from "@/lib/utils";

type Vec3 = { x: number; y: number; z: number };

export interface BonePhysicsEntry {
  shapeHalfExtents: Vec3;
  mass: number;
  joint: string;
  swingTwistLimits: Vec3;
  driveStiffness: number;
  driveDamping: number;
  driveMaxForce: number;
}

export interface BonePhysicsEditorProps {
  bones: BonePhysicsEntry[];
  joints: readonly Joint[];
  onChange(next: BonePhysicsEntry[]): void;
  onDragStart(): void;
  onDragEnd(): void;
}

const JOINT_OPTIONS = [
  { value: "fixed", label: "Fixed" },
  { value: "hinge", label: "Hinge" },
  { value: "swingtwist", label: "Swing-twist" },
  { value: "free", label: "Free" },
] as const;

const ZERO: Vec3 = { x: 0, y: 0, z: 0 };

export function BonePhysicsEditor({
  bones,
  joints,
  onChange,
  onDragStart,
  onDragEnd,
}: BonePhysicsEditorProps) {
  const [open, setOpen] = useState<Set<string>>(new Set());
  const [query, setQuery] = useState("");

  if (joints.length === 0) {
    return (
      <span className="px-0.5 text-[10px] text-muted-foreground">No skeleton on this entity.</span>
    );
  }

  const patch = (index: number, next: Partial<BonePhysicsEntry>): void => {
    onChange(bones.map((b, i) => (i === index ? { ...b, ...next } : b)));
  };
  const toggle = (id: string): void => {
    setOpen((prev) => {
      const next = new Set(prev);
      if (next.has(id)) {
        next.delete(id);
      } else {
        next.add(id);
      }
      return next;
    });
  };

  const q = query.trim().toLowerCase();
  const rows = bones
    .map((bone, index) => ({ bone, index, joint: joints[index] }))
    .filter((r) => r.joint !== undefined && (q === "" || r.joint.name.toLowerCase().includes(q)));

  return (
    <div className="flex flex-col gap-1.5">
      <Input
        value={query}
        onChange={(e) => setQuery(e.currentTarget.value)}
        placeholder="Filter bones…"
        className="h-7 rounded-sm bg-background px-2 py-0.5 text-[11px]"
      />
      <span className="px-0.5 text-[10px] text-muted-foreground">
        Auto-fit on import; applied when physics builds (next Play). Ragdoll blend is driven from
        the Physics panel.
      </span>
      <div className="flex flex-col gap-1">
        {rows.map((r) => {
          const joint = r.joint;
          if (joint === undefined) {
            return null;
          }
          const bone = r.bone;
          const isOpen = open.has(joint.id);
          const half = bone.shapeHalfExtents ?? ZERO;
          const limits = bone.swingTwistLimits ?? ZERO;
          return (
            <div key={joint.id} className="rounded border border-border/60">
              <button
                type="button"
                onClick={() => toggle(joint.id)}
                className="flex h-7 w-full items-center gap-1 border-b border-border/60 bg-muted/30 pr-2 pl-1 text-left"
              >
                {isOpen ? (
                  <ChevronDown className="size-3 flex-none text-muted-foreground" />
                ) : (
                  <ChevronRight className="size-3 flex-none text-muted-foreground" />
                )}
                <span className="min-w-0 flex-1 truncate text-[11px] font-medium text-foreground">
                  {joint.name}
                </span>
                <span className="flex-none font-mono text-[10px] text-muted-foreground">
                  {String(bone.joint ?? "swingtwist")}
                </span>
              </button>
              {isOpen ? (
                <div className="flex flex-col gap-1.5 px-2 py-1.5">
                  <Row label="Half extents">
                    <VectorEditor
                      axes={["x", "y", "z"]}
                      value={half}
                      step={0.01}
                      onChange={(p) => {
                        const m = { ...half, ...p };
                        patch(r.index, {
                          shapeHalfExtents: {
                            x: Math.max(0, m.x),
                            y: Math.max(0, m.y),
                            z: Math.max(0, m.z),
                          },
                        });
                      }}
                      onDragStart={onDragStart}
                      onDragEnd={onDragEnd}
                    />
                  </Row>
                  <Row label="Mass">
                    <NumberDrag
                      value={bone.mass ?? 0}
                      min={0}
                      step={0.05}
                      onChange={(v) => patch(r.index, { mass: v })}
                      onDragStart={onDragStart}
                      onDragEnd={onDragEnd}
                    />
                  </Row>
                  <Row label="Joint">
                    <EnumField
                      value={String(bone.joint ?? "swingtwist")}
                      options={JOINT_OPTIONS}
                      onChange={(v) => patch(r.index, { joint: v })}
                    />
                  </Row>
                  <Row label="Limits (°)">
                    <VectorEditor
                      axes={["x", "y", "z"]}
                      step={0.5}
                      value={{
                        x: limits.x * RAD_TO_DEG,
                        y: limits.y * RAD_TO_DEG,
                        z: limits.z * RAD_TO_DEG,
                      }}
                      onChange={(p) => {
                        const wire = Object.fromEntries(
                          Object.entries(p).map(([a, v]) => [a, v * DEG_TO_RAD]),
                        );
                        patch(r.index, { swingTwistLimits: { ...limits, ...wire } as Vec3 });
                      }}
                      onDragStart={onDragStart}
                      onDragEnd={onDragEnd}
                    />
                  </Row>
                  <Row label="Drive stiff">
                    <NumberDrag
                      value={bone.driveStiffness ?? 0}
                      min={0}
                      step={0.5}
                      onChange={(v) => patch(r.index, { driveStiffness: v })}
                      onDragStart={onDragStart}
                      onDragEnd={onDragEnd}
                    />
                  </Row>
                  <Row label="Drive damp">
                    <NumberDrag
                      value={bone.driveDamping ?? 0}
                      min={0}
                      step={0.5}
                      onChange={(v) => patch(r.index, { driveDamping: v })}
                      onDragStart={onDragStart}
                      onDragEnd={onDragEnd}
                    />
                  </Row>
                  <Row label="Drive max force">
                    <NumberDrag
                      value={bone.driveMaxForce ?? 0}
                      min={0}
                      step={1}
                      onChange={(v) => patch(r.index, { driveMaxForce: v })}
                      onDragStart={onDragStart}
                      onDragEnd={onDragEnd}
                    />
                  </Row>
                </div>
              ) : null}
            </div>
          );
        })}
        {rows.length === 0 ? (
          <span className="px-0.5 text-[10px] italic text-muted-foreground">No bones match.</span>
        ) : null}
      </div>
    </div>
  );
}

function Row({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="grid grid-cols-[88px_1fr] items-center gap-1.5">
      <Label className="truncate text-[11px] font-normal text-muted-foreground">{label}</Label>
      <div className="min-w-0">{children}</div>
    </div>
  );
}
