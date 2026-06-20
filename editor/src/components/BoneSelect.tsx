/// A bone-index dropdown for a rig-joint reference (a FootIk chain joint). The wire value is an
/// i32 index into SkinnedMeshComponent.bones; -1 = unset. Options are the resolved joint names,
/// so the user never types a raw index. Discrete change — the Inspector records one undo entry.
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";

/// One skeleton joint: its index into SkinnedMesh.bones, the bone entity uuid (a stable key),
/// and the resolved display name.
export interface Joint {
  index: number;
  id: string;
  name: string;
}

export interface BoneSelectProps {
  value: number;
  joints: readonly Joint[];
  onChange(next: number): void;
}

const NONE = "-1";

export function BoneSelect({ value, joints, onChange }: BoneSelectProps) {
  return (
    <Select value={String(value)} onValueChange={(v) => onChange(Number(v))}>
      <SelectTrigger size="sm" className="h-7 w-full font-mono text-[11px]">
        <SelectValue placeholder="(none)" />
      </SelectTrigger>
      <SelectContent>
        <SelectItem value={NONE} className="text-[11px]">
          (none)
        </SelectItem>
        {joints.map((j) => (
          <SelectItem key={j.id} value={String(j.index)} className="text-[11px]">
            {j.name}
          </SelectItem>
        ))}
      </SelectContent>
    </Select>
  );
}
