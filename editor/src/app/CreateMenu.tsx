/// A reusable entity-add dropdown listing every spawn preset, wired to
/// `client.addEntity(preset)`, plus a "Named Empty…" flow that opens a small inline
/// input and calls `client.createEntity(name)`. The engine auto-selects the new
/// entity, so on success we mirror that locally (optimistic `selectEntity`) and the
/// reconcile poll's sceneVersion bump refreshes the hierarchy list. A rejected call
/// surfaces in the inline flash beside the trigger.
///
/// The menu and the named-empty popover live in the left column (the Hierarchy
/// header) and anchor there.
import { useState } from "react";
import type { LucideIcon } from "lucide-react";
import { Box, Camera, CircleDashed, Flashlight, Lightbulb, Orbit, Plus, Sun } from "lucide-react";
import { client, type EntityPreset } from "../control/client";
import { useEditorStore } from "../state/store";
import { errorText, useFlash } from "../lib/flash";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import { Popover, PopoverAnchor, PopoverContent } from "@/components/ui/popover";

/// The single hand-maintained surface mapping menu labels to `add-entity`
/// presets (the preset union itself comes from the typed client / protocol).
/// Icons follow the C++ Lucide SVG mapping (editor_app.cppm:101-107): cube=Box,
/// point-light=Lightbulb, spot-light=Flashlight, camera=Camera (directional-light
/// uses Sun, empty uses a neutral marker). Exported so the phase-8 menu bar's
/// The list is kept separate from the component so future add surfaces can reuse it.
export const CREATE_PRESETS: { label: string; preset: EntityPreset; icon: LucideIcon }[] = [
  { label: "Empty", preset: "empty", icon: CircleDashed },
  { label: "Cube", preset: "cube", icon: Box },
  { label: "Point Light", preset: "point-light", icon: Lightbulb },
  { label: "Spot Light", preset: "spot-light", icon: Flashlight },
  { label: "Directional Light", preset: "directional-light", icon: Sun },
  { label: "Camera", preset: "camera", icon: Camera },
  { label: "Reflection Probe", preset: "reflection-probe", icon: Orbit },
];

export function CreateMenu() {
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const selectEntity = useEditorStore((s) => s.selectEntity);
  const { message, flash } = useFlash();
  const [namingOpen, setNamingOpen] = useState(false);

  const ready = phase === "ready";

  const create = (preset: EntityPreset): void => {
    void client
      .addEntity(preset)
      .then((ref) => {
        selectEntity(ref.id);
      })
      .catch((err: unknown) => flash(errorText(err)));
  };

  const createNamed = (name: string): void => {
    setNamingOpen(false);
    const trimmed = name.trim();
    if (trimmed === "") {
      return;
    }
    void client
      .createEntity(trimmed)
      .then((ref) => {
        selectEntity(ref.id);
      })
      .catch((err: unknown) => flash(errorText(err)));
  };

  return (
    <div className="flex min-w-0 items-center gap-2">
      <Popover open={namingOpen} onOpenChange={setNamingOpen}>
        <DropdownMenu>
          <PopoverAnchor asChild>
            <DropdownMenuTrigger asChild>
              <Button type="button" size="xs" variant="outline" disabled={!ready}>
                <Plus />
                <span>Add</span>
              </Button>
            </DropdownMenuTrigger>
          </PopoverAnchor>
          <DropdownMenuContent align="end" className="min-w-40">
            {CREATE_PRESETS.map(({ label, preset, icon: Icon }) => (
              <DropdownMenuItem key={preset} onSelect={() => create(preset)}>
                <Icon className="size-4 text-muted-foreground" />
                {label}
              </DropdownMenuItem>
            ))}
            <DropdownMenuSeparator />
            <DropdownMenuItem onSelect={() => setNamingOpen(true)}>
              <CircleDashed className="size-4 text-muted-foreground" />
              Named Empty…
            </DropdownMenuItem>
          </DropdownMenuContent>
        </DropdownMenu>
        <PopoverContent align="end" className="w-56 p-2">
          <NamedEmptyForm onCommit={createNamed} onCancel={() => setNamingOpen(false)} />
        </PopoverContent>
      </Popover>
      {message ? <span className="truncate text-xs text-destructive">{message}</span> : null}
    </div>
  );
}

/// Inline name entry for a named empty entity. Autofocuses; commits on Enter or the
/// Create button, cancels on Escape.
function NamedEmptyForm({
  onCommit,
  onCancel,
}: {
  onCommit(name: string): void;
  onCancel(): void;
}) {
  const [name, setName] = useState("");
  return (
    <form
      className="flex flex-col gap-2"
      onSubmit={(e) => {
        e.preventDefault();
        onCommit(name);
      }}
    >
      <Input
        autoFocus
        value={name}
        placeholder="Entity name"
        className="h-8 text-sm"
        onChange={(e) => setName(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === "Escape") {
            e.preventDefault();
            onCancel();
          }
        }}
      />
      <Button type="submit" size="xs" disabled={name.trim() === ""}>
        Create
      </Button>
    </form>
  );
}
