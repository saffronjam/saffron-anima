/// Generic enum/string combo. For an enum field, pass `options` and it renders a
/// shadcn `Select`. For a `uuid` field (no options yet), it is the phase-7 hand-off
/// slot: it shows the raw Uuid string (a `string`, never `Number()`-parsed) plus a
/// `(none)` clear button so the field is editable today; phase-7's `AssetPicker`
/// replaces this with a thumbnail combo + drag-drop target with no change to
/// `fieldRenderer` or `InspectorPanel`.
import { X } from "lucide-react";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";

const NONE_UUID = "0";

export interface ComboOption {
  value: string;
  label: string;
}

export interface ComboFieldProps {
  /// Current value as a string (a Uuid for asset fields; an enum literal otherwise).
  value: string;
  /// Enum options; omitted for uuid fields (phase-7 fills the asset options).
  options?: readonly ComboOption[];
  onChange(value: string): void;
}

export function ComboField({ value, options, onChange }: ComboFieldProps) {
  if (options && options.length > 0) {
    return (
      <Select value={value} onValueChange={onChange}>
        <SelectTrigger size="sm" className="h-7 w-full font-mono text-[11px]">
          <SelectValue />
        </SelectTrigger>
        <SelectContent>
          {options.map((opt) => (
            <SelectItem key={opt.value} value={opt.value}>
              {opt.label}
            </SelectItem>
          ))}
        </SelectContent>
      </Select>
    );
  }

  // uuid placeholder slot (phase-7 swaps in the AssetPicker). 0 == (none).
  const isNone = value === NONE_UUID || value === "";
  return (
    <div className="flex items-center gap-1 rounded-sm border border-border bg-background py-0.5 pr-1 pl-1.5">
      <span
        className="min-w-0 flex-1 truncate font-mono text-[11px] text-foreground"
        title={value}
      >
        {isNone ? "(none)" : value}
      </span>
      <button
        type="button"
        className="flex size-4 flex-none items-center justify-center rounded-sm text-muted-foreground hover:bg-accent hover:text-foreground disabled:cursor-default disabled:opacity-30"
        disabled={isNone}
        title="Clear"
        onClick={() => onChange(NONE_UUID)}
      >
        <X className="size-3" />
      </button>
    </div>
  );
}
