/// Enum dropdown for a string-valued component field (RigidbodyComponent.motion,
/// ColliderComponent.shape, BonePhysics.joint). The wire value is the raw lowercase
/// string the serde emits; the visible label is Sentence case. A discrete change fires
/// `onChange` once, so the Inspector's discrete-undo path records one entry on its own —
/// no drag bracket. Mirrors the AA / view-mode Selects in RenderPanel.
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";

export interface EnumOption {
  /// The lowercase wire string (sent verbatim to set-component).
  value: string;
  /// The Sentence-case label shown in the menu.
  label: string;
}

export interface EnumFieldProps {
  value: string;
  options: readonly EnumOption[];
  onChange(next: string): void;
}

export function EnumField({ value, options, onChange }: EnumFieldProps) {
  return (
    <Select value={value} onValueChange={onChange}>
      <SelectTrigger size="sm" className="h-7 w-full font-mono text-[11px]">
        <SelectValue />
      </SelectTrigger>
      <SelectContent>
        {options.map((o) => (
          <SelectItem key={o.value} value={o.value} className="text-[11px]">
            {o.label}
          </SelectItem>
        ))}
      </SelectContent>
    </Select>
  );
}
