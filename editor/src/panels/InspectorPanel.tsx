/// The Inspector panel: registry-driven and fully data-driven.
/// It reads the live `inspect` result from the store
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
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import type { CSSProperties, PointerEvent } from "react";
import { ArrowDownAZ, GripVertical, X } from "lucide-react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { makeCoalescer, type Coalescer } from "../control/coalesce";
import { errorText, notifyError } from "../lib/flash";
import { renderField, resolveHint } from "../components/fieldRenderer";
import { ScriptSlots } from "../components/ScriptSlots";
import { BoneMaskField } from "../components/BoneMaskField";
import { FootChainsEditor, type FootChain } from "../components/FootChainsEditor";
import { BonePhysicsEditor, type BonePhysicsEntry } from "../components/BonePhysicsEditor";
import type { Material, ScriptSlot, Transform } from "../protocol";
import { Button } from "@/components/ui/button";
import { Label } from "@/components/ui/label";
import { humanizeComponentName, humanizeFieldName } from "@/lib/humanize";
import { Separator } from "@/components/ui/separator";
import { ScrollArea } from "@/components/ui/scroll-area";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";
import { logRender } from "../lib/renderLog";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import {
  COMPONENT_ORDER,
  canonicalComponentNames,
  orderedComponentNames,
} from "../lib/componentOrder";

/// Components that cannot be removed. Name/Transform are the entity baseline;
/// ModelInstance/SkinnedMesh are import-managed identity (set when a model/rig is
/// instantiated, not addable by hand), so removing them would strand a rig with no way
/// back. Everything else shows a Remove control.
const NON_REMOVABLE = new Set<string>(["Name", "Transform", "ModelInstance", "SkinnedMesh"]);

/// Components NOT offered in the Add Component menu. MaterialSet's slots come from a
/// multi-material import (an empty one has nothing to edit); ModelInstance/SkinnedMesh
/// are written by model/rig import, never created on a bare entity.
const NON_ADDABLE = new Set<string>(["Name", "MaterialSet", "ModelInstance", "SkinnedMesh"]);

/// The Add Component list, in known order, minus the non-addable set.
const ADDABLE_COMPONENTS = COMPONENT_ORDER.filter((c) => !NON_ADDABLE.has(c));

/// Components only addable on a skinned entity: the rig sidecars index SkinnedMesh.bones,
/// and the animation player / foot-IK have no meaning without a skeleton to drive.
const RIG_ONLY = new Set<string>(["KinematicBones", "BonePhysics", "AnimationPlayer", "FootIk"]);
const SECTION_DRAG_THRESHOLD_PX = 4;

interface ComponentDragState {
  id: string;
  startY: number;
  currentY: number;
  startScrollTop: number;
  currentScrollTop: number;
  dragging: boolean;
  startIndex: number;
  previewIndex: number;
  height: number;
  order: string[];
  centers: Record<string, number>;
}

/// A non-editable label/value row, matching the field grid's two-column layout. Used by the
/// rig bodies (SkinnedMesh, FootIk, KinematicBones) for import-derived references shown by
/// resolved name rather than as an editable raw-id box.
function ReadonlyRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="grid grid-cols-[78px_1fr] items-center gap-1.5">
      <Label className="truncate text-[11px] font-normal text-muted-foreground">{label}</Label>
      <span className="min-w-0 truncate rounded-sm bg-muted/40 px-1.5 py-1 font-mono text-[11px] text-foreground">
        {value}
      </span>
    </div>
  );
}

export function InspectorPanel() {
  logRender("InspectorPanel");
  const selectedId = useEditorStore((s) => s.selectedId);
  const inspected = useEditorStore((s) => s.componentsBySelected);
  const selectionVersion = useEditorStore((s) => s.selectionVersion);
  const applyOptimisticComponent = useEditorStore((s) => s.applyOptimisticComponent);
  const focusComponent = useEditorStore((s) => s.focusComponent);
  const setFocusComponent = useEditorStore((s) => s.setFocusComponent);
  // Catalog + entity list, used to resolve the read-only id references in the rig bodies
  // (SkinnedMesh mesh/rootBone/joints, FootIk chains, KinematicBones driven) to names.
  const assets = useEditorStore((s) => s.assets);
  const entities = useEditorStore((s) => s.entities);
  const [componentDrag, setComponentDrag] = useState<ComponentDragState | null>(null);
  const scrollAreaRef = useRef<HTMLDivElement>(null);
  const settleRef = useRef<Map<string, number> | null>(null);

  // Per-(component,field) coalescers, rebuilt when the selection changes so a stale
  // closure never targets the wrong entity.
  const coalescers = useRef(new Map<string, Coalescer<object>>());
  useEffect(() => {
    coalescers.current.clear();
  }, [selectionVersion, selectedId]);

  // Undo capture: the in-flight field/slot gesture's prior snapshot + target entity,
  // recorded as one undo entry at the gesture's end. Distinct refs so a field scrub and
  // a slot scrub never alias.
  const fieldGesture = useRef<{
    component: string;
    field: string;
    prior: Record<string, unknown>;
    id: string;
  } | null>(null);
  const slotGesture = useRef<{
    slotIndex: number;
    field: string;
    prior: Record<string, unknown>;
    id: string;
  } | null>(null);

  // Consume the one-shot "jump to component" signal from the hierarchy subrows:
  // scroll the section into view when present, and always clear the signal so a
  // stale value never fires on a later render (component absent, selection raced).
  const sectionRefs = useRef(new Map<string, HTMLElement>());
  useLayoutEffect(() => {
    const before = settleRef.current;
    if (!before) {
      return;
    }
    settleRef.current = null;
    const nodes = [...sectionRefs.current.entries()];
    for (const [, node] of nodes) {
      node.style.transition = "none";
    }
    const settles: [HTMLElement, number][] = [];
    for (const [component, node] of nodes) {
      const top = before.get(component);
      if (top === undefined) {
        continue;
      }
      const diff = top - node.getBoundingClientRect().top;
      if (Math.abs(diff) >= 0.5) {
        settles.push([node, diff]);
      }
    }
    for (const [, node] of nodes) {
      node.style.transition = "";
    }
    for (const [node, diff] of settles) {
      node.animate([{ transform: `translateY(${diff}px)` }, { transform: "none" }], {
        duration: 150,
        easing: "ease-out",
      });
    }
  });

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
    () => (componentsObj ? orderedComponentNames(componentsObj, inspected?.componentOrder) : []),
    [componentsObj, inspected?.componentOrder],
  );
  // The two rig sidecars are meaningless without a skeleton: only offer them to add when
  // the entity carries a SkinnedMesh. (They keep their COMPONENT_ORDER slot so an
  // already-present section still renders — this gates add-availability, not visibility.)
  const hasSkin = !!componentsObj && "SkinnedMesh" in componentsObj;
  const missing = useMemo(
    () =>
      ADDABLE_COMPONENTS.filter(
        (c) => !(componentsObj && c in componentsObj) && (!RIG_ONLY.has(c) || hasSkin),
      ),
    [componentsObj, hasSkin],
  );

  const scrollViewport = (): HTMLElement | null =>
    scrollAreaRef.current?.querySelector(
      "[data-radix-scroll-area-viewport], [data-slot='scroll-area-viewport']",
    ) ?? null;

  function insertionIndexForPointer(
    drag: ComponentDragState,
    y: number,
    scrollTop: number,
  ): number {
    const scrollDelta = scrollTop - drag.startScrollTop;
    const withoutMoving = drag.order.filter((component) => component !== drag.id);
    for (let i = 0; i < withoutMoving.length; i += 1) {
      const component = withoutMoving[i];
      const center = drag.centers[component];
      if (center !== undefined && y + scrollDelta < center) {
        return i;
      }
    }
    return withoutMoving.length;
  }

  useEffect(() => {
    const viewport = scrollViewport();
    if (!viewport || !componentDrag) {
      return;
    }
    const onScroll = (): void => {
      setComponentDrag((drag) => {
        if (!drag) {
          return null;
        }
        const scrollTop = viewport.scrollTop;
        const delta = drag.currentY - drag.startY + (scrollTop - drag.startScrollTop);
        const dragging = drag.dragging || Math.abs(delta) >= SECTION_DRAG_THRESHOLD_PX;
        return {
          ...drag,
          currentScrollTop: scrollTop,
          dragging,
          previewIndex: dragging
            ? insertionIndexForPointer(drag, drag.currentY, scrollTop)
            : drag.previewIndex,
        };
      });
    };
    viewport.addEventListener("scroll", onScroll, { passive: true });
    return () => viewport.removeEventListener("scroll", onScroll);
  }, [componentDrag]);

  if (!selectedId || !inspected || !componentsObj) {
    return (
      <div className="flex h-full min-h-0 flex-col">
        <div className="min-h-0 flex-1 p-3.5 text-center italic text-muted-foreground">
          No entity selected
        </div>
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

  // Route a write to the right command for an explicit entity id. `payload` is the full
  // component DTO; the merge helpers (Transform/Material) take the changed field, a uuid
  // field its assign command, everything else the whole DTO (set-component does not
  // merge). Used live by `sendWrite` and by undo/redo replay with a captured id — so a
  // replay always targets the edited entity, never the live selection.
  const applyWrite = (
    id: string,
    component: string,
    field: string,
    payload: object,
    smooth: boolean,
  ): Promise<unknown> => {
    const dto = payload as Record<string, unknown>;
    const hint = resolveHint(component, field, dto[field]);
    if (hint.kind === "uuid") {
      const assetId = String(dto[field] ?? "0");
      if (component === "Mesh" && field === "mesh") {
        return client.assignAsset(id, "mesh", assetId);
      }
      if (component === "Material" && field === "albedoTexture") {
        return client.assignAsset(id, "albedo", assetId);
      }
      if (component === "Material" && field === "metallicRoughnessTexture") {
        return client.assignAsset(id, "metallic-roughness", assetId);
      }
      return client.setComponentField(id, component, field, assetId);
    }
    if (component === "Transform") {
      return client.setTransform(id, { [field]: dto[field] } as Partial<Transform>, smooth);
    }
    if (component === "Material") {
      return client.setMaterial(id, { [field]: dto[field] } as Partial<Material>, smooth);
    }
    return client.setComponent(id, component, dto);
  };

  // The live send for the field coalescer: current selection + drag-smoothing. Mid-drag
  // sends animate toward the value; the post-release re-push goes out exact.
  const sendWrite = (component: string, field: string, payload: object): Promise<unknown> => {
    const id = useEditorStore.getState().selectedId;
    if (!id) {
      return Promise.resolve();
    }
    return applyWrite(id, component, field, payload, useEditorStore.getState().dragActive);
  };

  const setDragActive = useEditorStore.getState().setDragActive;
  const pushEdit = useEditorStore.getState().pushEdit;

  const applyComponentOrderOptimistic = (order: string[]): void => {
    const current = useEditorStore.getState().componentsBySelected;
    if (!current) {
      return;
    }
    useEditorStore.getState().setComponentsBySelected({ ...current, componentOrder: order });
  };

  const applyComponentOrder = (id: string, order: string[]): Promise<unknown> =>
    client.setComponentOrder(id, order);

  const recordComponentOrderEdit = (id: string, prior: string[], after: string[]): void => {
    if (JSON.stringify(prior) === JSON.stringify(after)) {
      return;
    }
    pushEdit(
      {
        label: "Reorder components",
        selectionId: id,
        undo: () => applyComponentOrder(id, prior),
        redo: () => applyComponentOrder(id, after),
      },
      "scene",
    );
  };

  const commitComponentOrder = (next: string[]): void => {
    const id = selectedId;
    const prior = [...names];
    if (JSON.stringify(prior) === JSON.stringify(next)) {
      return;
    }
    applyComponentOrderOptimistic(next);
    void applyComponentOrder(id, next)
      .then(() => recordComponentOrderEdit(id, prior, next))
      .catch((err: unknown) => {
        applyComponentOrderOptimistic(prior);
        notifyError(errorText(err));
      });
  };

  const moveComponentToIndex = (component: string, index: number): void => {
    const without = names.filter((name) => name !== component);
    const insertAt = Math.min(Math.max(index, 0), without.length);
    commitComponentOrder([...without.slice(0, insertAt), component, ...without.slice(insertAt)]);
  };

  const beginComponentDrag = (component: string, event: PointerEvent<HTMLButtonElement>): void => {
    if (event.button !== 0) {
      return;
    }
    const node = sectionRefs.current.get(component);
    if (!node) {
      return;
    }
    const order = [...names];
    const startIndex = order.indexOf(component);
    if (startIndex < 0) {
      return;
    }
    const centers: Record<string, number> = {};
    for (const candidate of order) {
      const section = sectionRefs.current.get(candidate);
      if (section) {
        const rect = section.getBoundingClientRect();
        centers[candidate] = rect.top + rect.height / 2;
      }
    }
    const rect = node.getBoundingClientRect();
    const scrollTop = scrollViewport()?.scrollTop ?? 0;
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    setDragActive(true);
    setComponentDrag({
      id: component,
      startY: event.clientY,
      currentY: event.clientY,
      startScrollTop: scrollTop,
      currentScrollTop: scrollTop,
      dragging: false,
      startIndex,
      previewIndex: startIndex,
      height: rect.height,
      order,
      centers,
    });
  };

  const moveComponentDrag = (event: PointerEvent<HTMLButtonElement>): void => {
    if (!componentDrag) {
      return;
    }
    const scrollTop = scrollViewport()?.scrollTop ?? componentDrag.currentScrollTop;
    const delta = event.clientY - componentDrag.startY + (scrollTop - componentDrag.startScrollTop);
    const dragging = componentDrag.dragging || Math.abs(delta) >= SECTION_DRAG_THRESHOLD_PX;
    setComponentDrag({
      ...componentDrag,
      currentY: event.clientY,
      currentScrollTop: scrollTop,
      dragging,
      previewIndex: dragging
        ? insertionIndexForPointer(componentDrag, event.clientY, scrollTop)
        : componentDrag.previewIndex,
    });
  };

  const endComponentDrag = (component: string, event: PointerEvent<HTMLButtonElement>): void => {
    if (!componentDrag || componentDrag.id !== component) {
      return;
    }
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
    const nextIndex = componentDrag.previewIndex;
    if (componentDrag.dragging) {
      const tops = new Map<string, number>();
      for (const [name, node] of sectionRefs.current) {
        tops.set(name, node.getBoundingClientRect().top);
      }
      settleRef.current = tops;
    }
    setComponentDrag(null);
    setDragActive(false);
    if (componentDrag.dragging) {
      moveComponentToIndex(componentDrag.id, nextIndex);
    }
  };

  const resetComponentDrag = (): void => {
    setComponentDrag(null);
    setDragActive(false);
  };

  const componentDragStyle = (component: string): CSSProperties | undefined => {
    if (!componentDrag?.dragging) {
      return undefined;
    }
    if (component === componentDrag.id) {
      const scrollDelta = componentDrag.currentScrollTop - componentDrag.startScrollTop;
      return {
        transform: `translateY(${componentDrag.currentY - componentDrag.startY + scrollDelta}px)`,
      };
    }
    const index = componentDrag.order.indexOf(component);
    if (
      componentDrag.previewIndex > componentDrag.startIndex &&
      index > componentDrag.startIndex &&
      index <= componentDrag.previewIndex
    ) {
      return { transform: `translateY(-${componentDrag.height}px)` };
    }
    if (
      componentDrag.previewIndex < componentDrag.startIndex &&
      index >= componentDrag.previewIndex &&
      index < componentDrag.startIndex
    ) {
      return { transform: `translateY(${componentDrag.height}px)` };
    }
    return undefined;
  };

  const onSortComponents = (): void => {
    commitComponentOrder(canonicalComponentNames(componentsObj));
  };

  // Record one scene-tab undo entry for a field edit; a no-op (prior === after) is
  // dropped. Undo/redo replay through `applyWrite` against the captured entity id.
  const recordFieldEdit = (
    id: string,
    component: string,
    field: string,
    prior: object,
    after: object,
  ): void => {
    if (JSON.stringify(prior) === JSON.stringify(after)) {
      return;
    }
    pushEdit(
      {
        label: humanizeFieldName(field),
        selectionId: id,
        undo: () => applyWrite(id, component, field, prior, false),
        redo: () => applyWrite(id, component, field, after, false),
      },
      "scene",
    );
  };

  // A field edit: optimistically overlay the patched DTO and push it through the
  // coalescer. A discrete edit (bool/uuid, or a text commit — no active gesture) records
  // one undo entry here; a gesture's ticks are skipped and recorded at its end.
  const onFieldChange = (component: string, field: string, next: unknown): void => {
    const current = (componentsObj[component] ?? {}) as Record<string, unknown>;
    const patched = { ...current, [field]: next };
    if (fieldGesture.current === null) {
      const id = useEditorStore.getState().selectedId;
      if (id) {
        recordFieldEdit(id, component, field, structuredClone(current), structuredClone(patched));
      }
    }
    applyOptimisticComponent(component, patched);
    coalescerFor(component, field).push(patched);
  };

  // A field gesture (scrub drag, or a text field's focus..blur): capture the prior DTO +
  // target entity now and gate the poll; one entry is recorded at the end.
  const onFieldDragStart = (component: string, field: string): void => {
    setDragActive(true);
    const id = useEditorStore.getState().selectedId;
    if (id) {
      const prior = structuredClone((componentsObj[component] ?? {}) as Record<string, unknown>);
      fieldGesture.current = { component, field, prior, id };
    }
  };

  // Release: ungate the poll, re-push the latest optimistic value (one exact, non-smooth
  // write), then record the gesture as a single undo entry. Read from the store, not the
  // render closure — the pointerup listener holds a ctx stale by release.
  const onFieldDragEnd = (component: string, field: string): void => {
    setDragActive(false);
    const components = useEditorStore.getState().componentsBySelected?.components as
      | Record<string, unknown>
      | undefined;
    const current = components?.[component];
    if (current) {
      coalescerFor(component, field).push({ ...(current as object) });
    }
    const gesture = fieldGesture.current;
    fieldGesture.current = null;
    if (gesture && gesture.component === component && gesture.field === field && current) {
      recordFieldEdit(
        gesture.id,
        component,
        field,
        gesture.prior,
        structuredClone(current as Record<string, unknown>),
      );
    }
  };

  // MaterialSet slots: edits route through the slot-aware set-material command rather
  // than the generic field machinery (the field lives at slots[i].field, not top-level).
  const slotCoalescerFor = (slotIndex: number, field: string): Coalescer<object> => {
    const key = `MaterialSet#${slotIndex}.${field}`;
    let c = coalescers.current.get(key);
    if (!c) {
      c = makeCoalescer<object>({
        send: (latest) => {
          const id = useEditorStore.getState().selectedId;
          if (!id) {
            return Promise.resolve();
          }
          const slotDto = latest as Record<string, unknown>;
          const smooth = useEditorStore.getState().dragActive;
          return client.setMaterial(
            id,
            { [field]: slotDto[field] } as Partial<Material>,
            smooth,
            slotIndex,
          );
        },
      });
      coalescers.current.set(key, c);
    }
    return c;
  };

  // Record one scene-tab undo entry for a MaterialSet slot edit (set-material with the
  // slot index), replayed against the captured entity id.
  const recordSlotEdit = (
    id: string,
    slotIndex: number,
    field: string,
    prior: Record<string, unknown>,
    after: Record<string, unknown>,
  ): void => {
    if (JSON.stringify(prior) === JSON.stringify(after)) {
      return;
    }
    const apply = (slot: Record<string, unknown>): Promise<unknown> =>
      client.setMaterial(id, { [field]: slot[field] } as Partial<Material>, false, slotIndex);
    pushEdit(
      {
        label: humanizeFieldName(field),
        selectionId: id,
        undo: () => apply(prior),
        redo: () => apply(after),
      },
      "scene",
    );
  };

  const onSlotFieldChange = (slotIndex: number, field: string, next: unknown): void => {
    const set = (componentsObj["MaterialSet"] ?? {}) as { slots?: Record<string, unknown>[] };
    const slots = (set.slots ?? []).map((s, i) => (i === slotIndex ? { ...s, [field]: next } : s));
    if (slotGesture.current === null) {
      const id = useEditorStore.getState().selectedId;
      const priorSlot = set.slots?.[slotIndex];
      if (id && priorSlot) {
        recordSlotEdit(
          id,
          slotIndex,
          field,
          structuredClone(priorSlot),
          structuredClone(slots[slotIndex] ?? {}),
        );
      }
    }
    applyOptimisticComponent("MaterialSet", { slots });
    slotCoalescerFor(slotIndex, field).push({ ...(slots[slotIndex] ?? {}) });
  };

  const onSlotFieldDragStart = (slotIndex: number, field: string): void => {
    setDragActive(true);
    const id = useEditorStore.getState().selectedId;
    const slot = ((componentsObj["MaterialSet"] ?? {}) as { slots?: Record<string, unknown>[] })
      .slots?.[slotIndex];
    if (id && slot) {
      slotGesture.current = { slotIndex, field, prior: structuredClone(slot), id };
    }
  };

  const onSlotFieldDragEnd = (slotIndex: number, field: string): void => {
    setDragActive(false);
    const components = useEditorStore.getState().componentsBySelected?.components as
      | Record<string, unknown>
      | undefined;
    const slot = (components?.["MaterialSet"] as { slots?: Record<string, unknown>[] } | undefined)
      ?.slots?.[slotIndex];
    if (slot) {
      slotCoalescerFor(slotIndex, field).push({ ...slot });
    }
    const gesture = slotGesture.current;
    slotGesture.current = null;
    if (gesture && gesture.slotIndex === slotIndex && gesture.field === field && slot) {
      recordSlotEdit(gesture.id, slotIndex, field, gesture.prior, structuredClone(slot));
    }
  };

  // Add/remove a component records its inverse only after the engine accepts it (a
  // rejected op records nothing). Remove captures the full prior body so undo restores
  // the user's values, not engine defaults.
  const onRemove = (component: string): void => {
    const id = selectedId;
    const priorOrder = [...names];
    const afterOrder = names.filter((name) => name !== component);
    const priorBody = structuredClone((componentsObj[component] ?? {}) as Record<string, unknown>);
    void client
      .removeComponent(id, component)
      .then(() => {
        pushEdit(
          {
            label: `Remove ${component}`,
            selectionId: id,
            undo: async () => {
              await client.addComponent(id, component);
              await client.setComponent(id, component, priorBody);
              await client.setComponentOrder(id, priorOrder);
            },
            redo: async () => {
              await client.removeComponent(id, component);
              await client.setComponentOrder(id, afterOrder);
            },
          },
          "scene",
        );
      })
      .catch((err: unknown) => notifyError(errorText(err)));
  };
  const onAdd = (component: string): void => {
    const id = selectedId;
    const afterOrder = [...names, component];
    void client
      .addComponent(id, component)
      .then(() => {
        pushEdit(
          {
            label: `Add ${component}`,
            selectionId: id,
            undo: () => client.removeComponent(id, component),
            redo: async () => {
              await client.addComponent(id, component);
              await client.setComponentOrder(id, afterOrder);
            },
          },
          "scene",
        );
      })
      .catch((err: unknown) => notifyError(errorText(err)));
  };

  // Re-fit the selected Collider to its mesh AABB. The engine bumps sceneVersion, so the
  // reconcile poll re-reads the now-fitted halfExtents/offset/sourceMesh — no optimistic
  // overlay needed. Not undoable (a derived geometry op, like the render-config toggles).
  const onFitCollider = (): void => {
    void client.fitCollider(selectedId).catch((err: unknown) => notifyError(errorText(err)));
  };

  // One section body per component, dispatched by name with early returns (not a
  // JSX ternary chain). Script and MaterialSet have structured slot bodies; Collider and
  // BonePhysics have minimal structured bodies; every other component is the generic grid.
  const componentBody = (component: string, dto: Record<string, unknown>): React.ReactElement => {
    if (component === "Script") {
      return (
        <ScriptSlots
          entityId={selectedId}
          scripts={(dto.scripts as ScriptSlot[] | undefined) ?? []}
        />
      );
    }
    if (component === "MaterialSet") {
      const slots = (dto.slots as Record<string, unknown>[] | undefined) ?? [];
      return (
        <>
          {slots.map((slot, slotIndex) => (
            <div key={slotIndex} className="rounded border border-border/60">
              <div className="border-b border-border/60 bg-muted/30 px-2 py-1 text-[11px] font-medium text-muted-foreground">
                Slot {slotIndex}
              </div>
              <div className="flex flex-col gap-1.5 px-2 py-1.5">
                {Object.entries(slot).map(([field, value]) => (
                  <div key={field} className="grid grid-cols-[78px_1fr] items-center gap-1.5">
                    <Label className="truncate text-[11px] font-normal text-muted-foreground">
                      {humanizeFieldName(field)}
                    </Label>
                    <div className="min-w-0">
                      {renderField(
                        "Material",
                        field,
                        value,
                        (next) => onSlotFieldChange(slotIndex, field, next),
                        {
                          onDragStart: () => onSlotFieldDragStart(slotIndex, field),
                          onDragEnd: () => onSlotFieldDragEnd(slotIndex, field),
                        },
                      )}
                    </div>
                  </div>
                ))}
              </div>
            </div>
          ))}
        </>
      );
    }
    // The generic field grid, shared by the default body and the Collider body (which
    // prepends an action row above it).
    const fieldGrid = (comp: string, body: Record<string, unknown>): React.ReactElement => (
      <>
        {Object.entries(body).map(([field, value]) => (
          <div key={field} className="grid grid-cols-[78px_1fr] items-center gap-1.5">
            <Label className="truncate text-[11px] font-normal text-muted-foreground">
              {humanizeFieldName(field)}
            </Label>
            <div className="min-w-0">
              {renderField(comp, field, value, (next) => onFieldChange(comp, field, next), {
                onDragStart: () => onFieldDragStart(comp, field),
                onDragEnd: () => onFieldDragEnd(comp, field),
              })}
            </div>
          </div>
        ))}
      </>
    );

    if (component === "Collider") {
      // Auto-fit substitutes for the interactive resize handles Saffron has no gizmo for;
      // a collider with no Rigidbody is a static body (the engine rule) — surface why.
      const isStatic = !("Rigidbody" in componentsObj);
      return (
        <>
          <div className="flex items-center justify-between gap-2 pb-0.5">
            <Button type="button" size="xs" variant="outline" onClick={onFitCollider}>
              Fit to mesh
            </Button>
            {isStatic ? (
              <span className="truncate text-[10px] text-muted-foreground">
                No Rigidbody — static body
              </span>
            ) : null}
          </div>
          {fieldGrid("Collider", dto)}
        </>
      );
    }

    // Resolve the read-only id references in the rig bodies to display names: mesh ids hit the
    // asset catalog; rootBone/bone ids are scene entities (joints carry a Name); foot-IK and
    // kinematic-bone arrays hold integer indices into the rig's SkinnedMesh.bones.
    const assetName = (id: unknown): string => {
      const s = String(id ?? "0");
      if (s === "0" || s === "") {
        return "(none)";
      }
      return assets.find((a) => a.id === s)?.name ?? s;
    };
    const entityName = (id: unknown): string => {
      const s = String(id ?? "0");
      if (s === "0" || s === "") {
        return "(none)";
      }
      return entities.find((e) => e.id === s)?.name ?? s;
    };
    const rigBones = (): string[] => {
      const skin = componentsObj["SkinnedMesh"] as { bones?: unknown } | undefined;
      const bones = skin && Array.isArray(skin.bones) ? (skin.bones as unknown[]) : [];
      return bones.map((b) => String(b));
    };
    // The rig's joints as {index, id (bone uuid), name} for the bone pickers and per-bone cards.
    const rigJoints = (): { index: number; id: string; name: string }[] =>
      rigBones().map((id, index) => ({
        index,
        id,
        name: entities.find((e) => e.id === id)?.name ?? `Joint ${index}`,
      }));

    if (component === "SkinnedMesh") {
      // Import-derived rig data is read-only: the bone uuid array and the inverse-bind matrices
      // are never hand-edited (and the matrices are a meaningless JSON blob), so show a resolved
      // mesh / root-bone / joint-count summary instead of a field grid.
      const joints = rigBones();
      return (
        <div className="flex flex-col gap-1.5">
          <ReadonlyRow label="Mesh" value={assetName(dto.mesh)} />
          <ReadonlyRow label="Root bone" value={entityName(dto.rootBone)} />
          <ReadonlyRow
            label="Joints"
            value={`${joints.length} ${joints.length === 1 ? "joint" : "joints"} (import order)`}
          />
          <span className="px-0.5 text-[10px] text-muted-foreground">
            Skeleton bindings are set on import and not editable here.
          </span>
        </div>
      );
    }

    if (component === "FootIk") {
      // enabled/groundHeight are scalars (the generic grid); chains[] is the two-bone IK limbs,
      // edited as add/remove cards with joints picked by name. Writes round-trip the whole FootIk
      // DTO through set-component (consumed by the animation evaluator on the next frame in Play).
      const chains = Array.isArray(dto.chains) ? (dto.chains as FootChain[]) : [];
      return (
        <div className="flex flex-col gap-1.5">
          {fieldGrid("FootIk", { enabled: dto.enabled, groundHeight: dto.groundHeight })}
          <FootChainsEditor
            chains={chains}
            joints={rigJoints()}
            onChange={(next) => onFieldChange("FootIk", "chains", next)}
            onDragStart={() => onFieldDragStart("FootIk", "chains")}
            onDragEnd={() => onFieldDragEnd("FootIk", "chains")}
          />
        </div>
      );
    }

    if (component === "KinematicBones") {
      // enabled is the toggle; driven[] is the joint subset that gets kinematic bodies (empty = all),
      // edited as a bone-mask. Built from this array at the next play edge.
      const driven = Array.isArray(dto.driven)
        ? (dto.driven as unknown[]).filter((v): v is number => typeof v === "number")
        : [];
      return (
        <div className="flex flex-col gap-1.5">
          {fieldGrid("KinematicBones", { enabled: dto.enabled })}
          <BoneMaskField
            value={driven}
            joints={rigJoints()}
            onChange={(next) => onFieldChange("KinematicBones", "driven", next)}
          />
        </div>
      );
    }

    if (component === "BonePhysics") {
      // Per-bone ragdoll/collision data, 1:1 with the skeleton: fixed-length cards labeled by joint
      // name. Edits round-trip the whole bones[] through set-component and apply when physics builds
      // at the next play edge.
      const bones = Array.isArray(dto.bones) ? (dto.bones as BonePhysicsEntry[]) : [];
      return (
        <BonePhysicsEditor
          bones={bones}
          joints={rigJoints()}
          onChange={(next) => onFieldChange("BonePhysics", "bones", next)}
          onDragStart={() => onFieldDragStart("BonePhysics", "bones")}
          onDragEnd={() => onFieldDragEnd("BonePhysics", "bones")}
        />
      );
    }

    return fieldGrid(component, dto);
  };

  return (
    <div className="flex h-full min-h-0 flex-col">
      <ScrollArea ref={scrollAreaRef} className="min-h-0 flex-1">
        <div className="flex flex-col gap-2 p-1.5">
          {names.map((component) => {
            const dto = (componentsObj[component] ?? {}) as Record<string, unknown>;
            const removable = !NON_REMOVABLE.has(component);
            const dragged = componentDrag?.dragging && componentDrag.id === component;
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
                className={[
                  "overflow-hidden rounded-md border border-border bg-background transition-[transform,border-color,box-shadow] duration-150 ease-out",
                  dragged ? "relative z-10 shadow-lg transition-none" : "",
                ].join(" ")}
                style={componentDragStyle(component)}
              >
                <header className="flex h-8 items-center justify-between border-b border-border bg-muted/50 pr-1 pl-1">
                  <div className="flex min-w-0 items-center gap-1">
                    <Tooltip>
                      <TooltipTrigger asChild>
                        <Button
                          type="button"
                          size="icon-xs"
                          variant="ghost"
                          className="cursor-grab text-muted-foreground active:cursor-grabbing"
                          onPointerDown={(event) => beginComponentDrag(component, event)}
                          onPointerMove={moveComponentDrag}
                          onPointerUp={(event) => endComponentDrag(component, event)}
                          onPointerCancel={resetComponentDrag}
                          onLostPointerCapture={resetComponentDrag}
                        >
                          <GripVertical />
                        </Button>
                      </TooltipTrigger>
                      <TooltipContent>Reorder {component}</TooltipContent>
                    </Tooltip>
                    <span className="truncate text-xs font-semibold tracking-wide text-foreground">
                      {humanizeComponentName(component)}
                    </span>
                  </div>
                  {removable ? (
                    <Tooltip>
                      <TooltipTrigger asChild>
                        <Button
                          type="button"
                          size="icon-xs"
                          variant="ghost"
                          className="text-muted-foreground hover:text-destructive"
                          onClick={() => onRemove(component)}
                        >
                          <X />
                        </Button>
                      </TooltipTrigger>
                      <TooltipContent>Remove {component}</TooltipContent>
                    </Tooltip>
                  ) : null}
                </header>
                <div className="flex flex-col gap-1.5 px-2 py-1.5">
                  {componentBody(component, dto)}
                </div>
              </section>
            );
          })}

          <Separator className="my-1" />

          <div className="flex items-center gap-1.5">
            <DropdownMenu>
              <DropdownMenuTrigger asChild>
                <Button
                  type="button"
                  variant="outline"
                  size="sm"
                  className="min-w-0 flex-1"
                  disabled={missing.length === 0}
                >
                  Add Component
                </Button>
              </DropdownMenuTrigger>
              <DropdownMenuContent
                align="start"
                className="w-(--radix-dropdown-menu-trigger-width)"
              >
                {missing.map((component) => (
                  <DropdownMenuItem key={component} onSelect={() => onAdd(component)}>
                    {humanizeComponentName(component)}
                  </DropdownMenuItem>
                ))}
              </DropdownMenuContent>
            </DropdownMenu>
            <Tooltip>
              <TooltipTrigger asChild>
                <Button
                  type="button"
                  variant="outline"
                  size="icon-sm"
                  className="flex-none"
                  onClick={onSortComponents}
                  disabled={
                    JSON.stringify(names) === JSON.stringify(canonicalComponentNames(componentsObj))
                  }
                >
                  <ArrowDownAZ />
                </Button>
              </TooltipTrigger>
              <TooltipContent>Sort components</TooltipContent>
            </Tooltip>
          </div>
        </div>
      </ScrollArea>
    </div>
  );
}
