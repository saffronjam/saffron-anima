/// Add/remove editor for FootIk.chains. Each chain is a two-bone IK limb: upper→mid→end joints
/// (indices into SkinnedMesh.bones, picked by name) plus a pole vector that orients the knee/elbow
/// plane. Joint picks and add/remove are discrete edits; the pole-vector scrub brackets a gesture.
import { Plus, X } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Label } from "@/components/ui/label";
import { VectorEditor } from "./VectorEditor";
import { BoneSelect, type Joint } from "./BoneSelect";

type Vec3 = { x: number; y: number; z: number };

export interface FootChain {
  upper: number;
  mid: number;
  end: number;
  poleVector: Vec3;
}

export interface FootChainsEditorProps {
  chains: FootChain[];
  joints: readonly Joint[];
  onChange(next: FootChain[]): void;
  onDragStart(): void;
  onDragEnd(): void;
}

const DEFAULT_POLE: Vec3 = { x: 0, y: 0, z: 1 };

export function FootChainsEditor({
  chains,
  joints,
  onChange,
  onDragStart,
  onDragEnd,
}: FootChainsEditorProps) {
  const patch = (index: number, next: Partial<FootChain>): void => {
    onChange(chains.map((c, i) => (i === index ? { ...c, ...next } : c)));
  };
  const seen = new Map<string, number>();

  return (
    <div className="flex flex-col gap-1.5">
      {chains.map((chain, index) => {
        const base = `${chain.upper}:${chain.mid}:${chain.end}`;
        const occ = seen.get(base) ?? 0;
        seen.set(base, occ + 1);
        const pole = chain.poleVector ?? { x: 0, y: 0, z: 0 };
        return (
          <div key={`${base}#${occ}`} className="rounded border border-border/60">
            <div className="flex h-7 items-center justify-between border-b border-border/60 bg-muted/30 pr-0.5 pl-2">
              <span className="text-[11px] font-medium text-muted-foreground">Chain {index}</span>
              <Button
                type="button"
                size="icon-xs"
                variant="ghost"
                className="text-muted-foreground hover:text-destructive"
                aria-label={`Remove chain ${index}`}
                onClick={() => onChange(chains.filter((_, i) => i !== index))}
              >
                <X />
              </Button>
            </div>
            <div className="flex flex-col gap-1.5 px-2 py-1.5">
              <ChainRow
                label="Upper"
                value={chain.upper}
                joints={joints}
                onChange={(v) => patch(index, { upper: v })}
              />
              <ChainRow
                label="Mid"
                value={chain.mid}
                joints={joints}
                onChange={(v) => patch(index, { mid: v })}
              />
              <ChainRow
                label="End"
                value={chain.end}
                joints={joints}
                onChange={(v) => patch(index, { end: v })}
              />
              <div className="grid grid-cols-[72px_1fr] items-center gap-1.5">
                <Label className="truncate text-[11px] font-normal text-muted-foreground">
                  Pole
                </Label>
                <VectorEditor
                  axes={["x", "y", "z"]}
                  value={pole}
                  step={0.05}
                  onChange={(p) => patch(index, { poleVector: { ...pole, ...p } })}
                  onDragStart={onDragStart}
                  onDragEnd={onDragEnd}
                />
              </div>
            </div>
          </div>
        );
      })}
      <Button
        type="button"
        size="sm"
        variant="outline"
        className="w-full"
        onClick={() =>
          onChange([...chains, { upper: -1, mid: -1, end: -1, poleVector: { ...DEFAULT_POLE } }])
        }
      >
        <Plus /> Add chain
      </Button>
    </div>
  );
}

function ChainRow({
  label,
  value,
  joints,
  onChange,
}: {
  label: string;
  value: number;
  joints: readonly Joint[];
  onChange(next: number): void;
}) {
  return (
    <div className="grid grid-cols-[72px_1fr] items-center gap-1.5">
      <Label className="truncate text-[11px] font-normal text-muted-foreground">{label}</Label>
      <BoneSelect value={value} joints={joints} onChange={onChange} />
    </div>
  );
}
