/// The Inspector panel: the React port of the C++ registry-driven `inspectorPanel`,
/// fully data-driven. It reads the live `inspect` result from the store
/// (`componentsBySelected`, kept fresh by the reconcile poll) and renders EVERY
/// present component's fields via `renderField` — there is NO per-component switch,
/// so a future engine-side `registerComponent` shows up here automatically (with a
/// value-shape fallback if it has no FIELD_HINTS entry yet).
///
/// Writes are read-modify-write: `set-component` rewrites the whole component (no
/// merge), so a single field edit sends the full DTO with that one field patched.
/// Transform/Material use the server-merge helpers instead; uuid fields use the
/// single-field merge. High-frequency edits (scrub/slider) funnel through a per-
/// (component,field) coalescer; the scrub brackets flip `store.dragActive` so the
/// reconcile poll won't clobber the optimistic value mid-drag.
import { useEffect, useMemo, useRef } from "react";
import { X } from "lucide-react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { makeCoalescer, type Coalescer } from "../control/coalesce";
import { errorText, useFlash } from "../lib/flash";
import { renderField, resolveHint } from "../components/fieldRenderer";
import type { Material, Transform } from "../protocol";
import { Button } from "@/components/ui/button";
import { Label } from "@/components/ui/label";
import { humanizeFieldName } from "@/lib/humanize";
import { Separator } from "@/components/ui/separator";
import { ScrollArea } from "@/components/ui/scroll-area";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";

/// Canonical component order (mirrors the C++ registry order / the `Components`
/// schema key order). Components present but not listed render in insertion order
/// after these. This is ordering only — never a per-component render switch.
const COMPONENT_ORDER = [
  "Name",
  "Transform",
  "Mesh",
  "Camera",
  "Material",
  "DirectionalLight",
  "PointLight",
  "SpotLight",
  "ReflectionProbe",
] as const;

/// Components that cannot be removed (parity with the C++ `removable=false` flag on
/// Name/Transform). Everything else shows a Remove control.
const NON_REMOVABLE = new Set<string>(["Name", "Transform"]);

/// Components the Inspector never renders: Relationship carries the hierarchy's
/// durable parent uuid, edited through the tree / `set-parent` — never as a raw field;
/// Bone is an empty joint tag (bone-ness shows in the outliner, not as a section).
const HIDDEN_COMPONENTS = new Set<string>(["Relationship", "Bone"]);

/// The full registered component set (for the Add Component list). Derived from the
/// known order; a regenerated schema with new components extends COMPONENT_ORDER.
const ADDABLE_COMPONENTS = COMPONENT_ORDER.filter((c) => c !== "Name");

/// Shared with the hierarchy's component subrows so the tree leaves and the
/// Inspector sections stay in lockstep (same order, same hidden set).
export function orderedComponentNames(components: Record<string, unknown>): string[] {
  const present = Object.keys(components).filter((c) => !HIDDEN_COMPONENTS.has(c));
  const known = COMPONENT_ORDER.filter((c) => present.includes(c));
  const extra = present.filter((c) => !COMPONENT_ORDER.includes(c as never));
  return [...known, ...extra];
}

export function InspectorPanel() {
  const selectedId = useEditorStore((s) => s.selectedId);
  const inspected = useEditorStore((s) => s.componentsBySelected);
  const selectionVersion = useEditorStore((s) => s.selectionVersion);
  const applyOptimisticComponent = useEditorStore((s) => s.applyOptimisticComponent);
  const focusComponent = useEditorStore((s) => s.focusComponent);
  const setFocusComponent = useEditorStore((s) => s.setFocusComponent);
  const { message, flash } = useFlash();

  // Per-(component,field) coalescers, rebuilt when the selection changes so a stale
  // closure never targets the wrong entity.
  const coalescers = useRef(new Map<string, Coalescer<object>>());
  useEffect(() => {
    coalescers.current.clear();
  }, [selectionVersion, selectedId]);

  // Consume the one-shot "jump to component" signal from the hierarchy subrows:
  // scroll the section into view when present, and always clear the signal so a
  // stale value never fires on a later render (component absent, selection raced).
  const sectionRefs = useRef(new Map<string, HTMLElement>());
  useEffect(() => {
    if (!focusComponent) {
      return;
    }
    sectionRefs.current
      .get(focusComponent)
      ?.scrollIntoView({ block: "nearest", behavior: "smooth" });
    setFocusComponent(null);
  }, [focusComponent, setFocusComponent]);

  const componentsObj = inspected?.components as Record<string, unknown> | undefined;
  const names = useMemo(
    () => (componentsObj ? orderedComponentNames(componentsObj) : []),
    [componentsObj],
  );
  const missing = useMemo(
    () => ADDABLE_COMPONENTS.filter((c) => !(componentsObj && c in componentsObj)),
    [componentsObj],
  );

  if (!selectedId || !inspected || !componentsObj) {
    return (
      <div className="flex h-full min-h-0 flex-col">
        <div className="min-h-0 flex-1 p-3.5 text-center italic text-muted-foreground">
          No entity selected
        </div>
        {message ? (
          <p className="flex-none border-t border-destructive/40 bg-destructive/10 px-2.5 py-1 text-[11px] text-destructive">
            {message}
          </p>
        ) : null}
      </div>
    );
  }

  // Resolve the coalescer for a (component,field) write, building it on first use.
  // The send routes by component to the right merge helper; for full-DTO components
  // the buffered value IS the full DTO (read-modify-write).
  const coalescerFor = (component: string, field: string): Coalescer<object> => {
    const key = `${component}.${field}`;
    let c = coalescers.current.get(key);
    if (!c) {
      c = makeCoalescer<object>({
        send: (latest) => sendWrite(component, field, latest),
      });
      coalescers.current.set(key, c);
    }
    return c;
  };

  // Route a buffered write to the right command. `payload` is the full patched DTO.
  const sendWrite = (component: string, field: string, payload: object): Promise<unknown> => {
    const id = useEditorStore.getState().selectedId;
    if (!id) {
      return Promise.resolve();
    }
    const dto = payload as Record<string, unknown>;
    const hint = resolveHint(component, field, dto[field]);

    // uuid fields write a string id (never Number()). Mesh.mesh / Material.albedoTexture
    // have the dedicated, minimal assign-asset command (adds the component if missing
    // and writes the slot); every other Uuid field uses the generic single-field merge.
    if (hint.kind === "uuid") {
      const assetId = String(dto[field] ?? "0");
      if (component === "Mesh" && field === "mesh") {
        return client.assignAsset(id, "mesh", assetId);
      }
      if (component === "Material" && field === "albedoTexture") {
        return client.assignAsset(id, "albedo", assetId);
      }
      return client.setComponentField(id, component, field, assetId);
    }
    // Transform/Material have server-side merge helpers — send only the changed field.
    if (component === "Transform") {
      return client.setTransform(id, { [field]: dto[field] } as Partial<Transform>);
    }
    if (component === "Material") {
      return client.setMaterial(id, { [field]: dto[field] } as Partial<Material>);
    }
    // Everything else: set-component does NOT merge, send the whole DTO.
    return client.setComponent(id, component, dto);
  };

  // A field edit: build the full patched DTO, overlay it optimistically, and push
  // it through the field's coalescer.
  const onFieldChange = (component: string, field: string, next: unknown): void => {
    const current = (componentsObj[component] ?? {}) as Record<string, unknown>;
    const patched = { ...current, [field]: next };
    applyOptimisticComponent(component, patched);
    coalescerFor(component, field).push(patched);
  };

  const setDragActive = useEditorStore.getState().setDragActive;
  const onDragStart = (): void => setDragActive(true);
  const onDragEnd = (): void => setDragActive(false);

  const onRemove = (component: string): void => {
    void client
      .removeComponent(selectedId, component)
      .catch((err: unknown) => flash(errorText(err)));
  };
  const onAdd = (component: string): void => {
    void client.addComponent(selectedId, component).catch((err: unknown) => flash(errorText(err)));
  };

  return (
    <div className="flex h-full min-h-0 flex-col">
      <ScrollArea className="min-h-0 flex-1">
        <div className="flex flex-col gap-2 p-1.5">
          {names.map((component) => {
            const dto = (componentsObj[component] ?? {}) as Record<string, unknown>;
            const removable = !NON_REMOVABLE.has(component);
            return (
              <section
                key={component}
                ref={(el) => {
                  if (el) {
                    sectionRefs.current.set(component, el);
                  } else {
                    sectionRefs.current.delete(component);
                  }
                }}
                className="overflow-hidden rounded-md border border-border bg-background"
              >
                <header className="flex h-8 items-center justify-between border-b border-border bg-muted/50 pr-1 pl-2.5">
                  <span className="text-xs font-semibold tracking-wide text-foreground">
                    {component}
                  </span>
                  {removable ? (
                    <Button
                      type="button"
                      size="icon-xs"
                      variant="ghost"
                      className="text-muted-foreground hover:text-destructive"
                      title={`Remove ${component}`}
                      onClick={() => onRemove(component)}
                    >
                      <X />
                    </Button>
                  ) : null}
                </header>
                <div className="flex flex-col gap-1.5 px-2 py-1.5">
                  {Object.entries(dto).map(([field, value]) => (
                    <div key={field} className="grid grid-cols-[78px_1fr] items-center gap-1.5">
                      <Label className="truncate text-[11px] font-normal text-muted-foreground">
                        {humanizeFieldName(field)}
                      </Label>
                      <div className="min-w-0">
                        {renderField(
                          component,
                          field,
                          value,
                          (next) => onFieldChange(component, field, next),
                          { onDragStart, onDragEnd },
                        )}
                      </div>
                    </div>
                  ))}
                </div>
              </section>
            );
          })}

          <Separator className="my-1" />

          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button
                type="button"
                variant="outline"
                size="sm"
                className="w-full"
                disabled={missing.length === 0}
              >
                Add Component
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="start" className="w-(--radix-dropdown-menu-trigger-width)">
              {missing.map((component) => (
                <DropdownMenuItem key={component} onSelect={() => onAdd(component)}>
                  {component}
                </DropdownMenuItem>
              ))}
            </DropdownMenuContent>
          </DropdownMenu>
        </div>
      </ScrollArea>
      {message ? (
        <p className="flex-none border-t border-destructive/40 bg-destructive/10 px-2.5 py-1 text-[11px] text-destructive">
          {message}
        </p>
      ) : null}
    </div>
  );
}
