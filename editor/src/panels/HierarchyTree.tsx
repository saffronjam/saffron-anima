/// The outliner tree: the entity forest built client-side from the flat store slice
/// (`buildTree` over `parentId`). Rows indent by
/// depth, show a twisty only when they have something to expand (child entities, or —
/// behind the header toggle — the selected row's read-only component subrows), select
/// on click, rename inline on double-click, delete on the Delete key, and reparent
/// by dragging one row onto another (`set-parent`); dropping onto self or a
/// descendant is rejected before any
/// round trip, and a root strip at the bottom unparents. Expand-state lives outside
/// the version-gated poll, so a scene mutation never collapses the tree.
///
/// Every drag affordance is in-flow sidebar DOM — no `setDragImage` layer, no portal'd
/// indicator — because the reparented X11 viewport child paints over anything floating.
/// The context menu stays Radix-anchored to the sidebar for the same reason.
import { useEffect, useMemo, useRef, useState } from "react";
import { ChevronRight } from "lucide-react";
import { useEditorStore, buildTree, reanchorPastBones, type TreeNode } from "../state/store";
import { matchesBinding } from "../lib/keybindings";
import { orderedComponentNames } from "./InspectorPanel";
import type { EntityListEntry } from "../protocol";
import { Input } from "@/components/ui/input";
import {
  ContextMenu,
  ContextMenuContent,
  ContextMenuItem,
  ContextMenuSeparator,
  ContextMenuSub,
  ContextMenuSubContent,
  ContextMenuSubTrigger,
  ContextMenuTrigger,
} from "@/components/ui/context-menu";
import { cn } from "@/lib/utils";

export interface TreeActions {
  onSelect(entity: EntityListEntry): void;
  onFocus(id: string): void;
  onCopy(id: string): void;
  onDelete(id: string): void;
  /// Reparent via the store's optimistic setParent; null detaches to root. The
  /// caller wraps rejections in its flash.
  onReparent(id: string, parentId: string | null): void;
  renamingId: string | null;
  onRenameStart(id: string): void;
  onRenameCommit(id: string, next: string): void;
  onRenameCancel(): void;
}

const DND_MIME = "application/x-saffron-entity";

/// True when `candidateId` is `rootId` or sits anywhere in its subtree, walking the
/// candidate's ancestry through `parentId` (bounded so corrupt data cannot loop).
function isInSubtree(entities: EntityListEntry[], rootId: string, candidateId: string): boolean {
  const parentOf = new Map(entities.map((e) => [e.id, e.parentId]));
  let cursor: string | undefined = candidateId;
  for (let steps = 0; cursor && cursor !== "0" && steps <= entities.length; steps++) {
    if (cursor === rootId) {
      return true;
    }
    cursor = parentOf.get(cursor);
  }
  return false;
}

export function HierarchyTree({ actions }: { actions: TreeActions }) {
  const entities = useEditorStore((s) => s.entities);
  const selectedId = useEditorStore((s) => s.selectedId);
  const setExpanded = useEditorStore((s) => s.setExpanded);
  const setDragActive = useEditorStore((s) => s.setDragActive);
  const hideBones = useEditorStore((s) => s.hideBones);
  const [draggingId, setDraggingId] = useState<string | null>(null);
  const [dropTargetId, setDropTargetId] = useState<string | null>(null);

  // The bone filter only shapes the RENDERED tree; drag validity and reparenting
  // keep working against the unfiltered entities, so a drop can never corrupt the
  // real ancestry through a hidden joint.
  const roots = useMemo(
    () => buildTree(hideBones ? reanchorPastBones(entities) : entities),
    [entities, hideBones],
  );

  // Reveal an externally selected row (viewport pick, `se select`): expand every
  // ancestor so the selection is never hidden inside a collapsed branch.
  const lastRevealed = useRef<string | null>(null);
  useEffect(() => {
    if (!selectedId || selectedId === lastRevealed.current) {
      return;
    }
    lastRevealed.current = selectedId;
    const parentOf = new Map(entities.map((e) => [e.id, e.parentId]));
    let cursor = parentOf.get(selectedId);
    for (let steps = 0; cursor && cursor !== "0" && steps <= entities.length; steps++) {
      setExpanded(cursor, true);
      cursor = parentOf.get(cursor);
    }
  }, [selectedId, entities, setExpanded]);

  const dragContext = {
    entities,
    draggingId,
    dropTargetId,
    begin: (id: string) => {
      setDraggingId(id);
      setDragActive(true);
    },
    end: () => {
      setDraggingId(null);
      setDropTargetId(null);
      setDragActive(false);
    },
    setDropTarget: setDropTargetId,
    drop: (targetId: string | null) => {
      if (draggingId) {
        actions.onReparent(draggingId, targetId);
      }
      setDraggingId(null);
      setDropTargetId(null);
    },
  };

  const draggingEntity = draggingId ? entities.find((e) => e.id === draggingId) : undefined;

  return (
    <div className="p-1" role="tree" aria-label="Scene entities">
      {entities.length === 0 ? (
        <div className="p-2.5 text-center italic text-muted-foreground">No entities</div>
      ) : (
        roots.map((node) => (
          <TreeRow
            key={node.entity.id}
            node={node}
            depth={0}
            actions={actions}
            drag={dragContext}
          />
        ))
      )}
      {draggingEntity?.parentId ? (
        <div
          className={cn(
            "mt-1 rounded-md border border-dashed border-border px-2.5 py-1.5 text-center text-xs text-muted-foreground",
            dropTargetId === "" && "border-primary text-primary",
          )}
          onDragOver={(e) => {
            e.preventDefault();
            dragContext.setDropTarget("");
          }}
          onDragLeave={() => dragContext.setDropTarget(null)}
          onDrop={(e) => {
            e.preventDefault();
            dragContext.drop(null);
          }}
        >
          Drop here to unparent
        </div>
      ) : null}
    </div>
  );
}

interface DragContext {
  entities: EntityListEntry[];
  draggingId: string | null;
  dropTargetId: string | null;
  begin(id: string): void;
  end(): void;
  setDropTarget(id: string | null): void;
  drop(targetId: string | null): void;
}

function TreeRow({
  node,
  depth,
  actions,
  drag,
}: {
  node: TreeNode;
  depth: number;
  actions: TreeActions;
  drag: DragContext;
}) {
  const entity = node.entity;
  const selectedId = useEditorStore((s) => s.selectedId);
  const expandedIds = useEditorStore((s) => s.expandedIds);
  const toggleExpanded = useEditorStore((s) => s.toggleExpanded);
  const showComponentSubrows = useEditorStore((s) => s.showComponentSubrows);
  const componentsBySelected = useEditorStore((s) => s.componentsBySelected);
  const expanded = expandedIds.has(entity.id);
  const hasChildren = node.children.length > 0;
  const selected = entity.id === selectedId;
  const renaming = entity.id === actions.renamingId;

  // Read-only component leaves for the SELECTED row only, sourced from the inspect
  // result the poll already keeps fresh — never an extra control call. The id guard
  // drops a mid-poll race where the inspect payload lags a selection change.
  const subrows =
    showComponentSubrows &&
    selected &&
    componentsBySelected &&
    componentsBySelected.id === entity.id
      ? orderedComponentNames(componentsBySelected.components as Record<string, unknown>)
      : [];
  const expandable = hasChildren || subrows.length > 0;

  // A row is a valid drop target unless it is the dragged row or inside its subtree.
  const validDropTarget =
    drag.draggingId !== null &&
    drag.draggingId !== entity.id &&
    !isInSubtree(drag.entities, drag.draggingId, entity.id);

  // Valid "Parent to…" targets: anything outside this row's own subtree.
  const parentTargets = drag.entities.filter(
    (e) => e.id !== entity.id && !isInSubtree(drag.entities, entity.id, e.id),
  );

  return (
    <>
      <ContextMenu>
        <ContextMenuTrigger asChild>
          {renaming ? (
            <div style={{ paddingLeft: depth * 14 }}>
              <RenameRow
                initial={entity.name}
                onCommit={(next) => actions.onRenameCommit(entity.id, next)}
                onCancel={actions.onRenameCancel}
              />
            </div>
          ) : (
            <div
              role="treeitem"
              aria-selected={selected}
              aria-expanded={expandable ? expanded : undefined}
              draggable
              tabIndex={0}
              className={cn(
                "flex w-full cursor-default items-center gap-0.5 rounded-md px-1 py-1.5 text-left text-sm",
                selected ? "bg-primary text-primary-foreground" : "text-foreground hover:bg-accent",
                drag.dropTargetId === entity.id && validDropTarget && "ring-1 ring-primary",
              )}
              style={{ paddingLeft: depth * 14 + 4 }}
              onClick={() => actions.onSelect(entity)}
              onMouseDown={() => {
                if (actions.renamingId && actions.renamingId !== entity.id) {
                  actions.onRenameCancel();
                }
              }}
              onDoubleClick={() => actions.onRenameStart(entity.id)}
              // The configured delete key on the focused row (clicking focuses it)
              // runs the same delete as the context menu item.
              onKeyDown={(e) => {
                if (matchesBinding(e, "hierarchy.delete", useEditorStore.getState().keyBindings)) {
                  e.preventDefault();
                  actions.onDelete(entity.id);
                }
              }}
              onDragStart={(e) => {
                e.dataTransfer.setData(DND_MIME, entity.id);
                e.dataTransfer.effectAllowed = "move";
                drag.begin(entity.id);
              }}
              onDragEnd={drag.end}
              onDragOver={(e) => {
                if (validDropTarget) {
                  e.preventDefault();
                  e.dataTransfer.dropEffect = "move";
                  drag.setDropTarget(entity.id);
                }
              }}
              onDragLeave={() => {
                if (drag.dropTargetId === entity.id) {
                  drag.setDropTarget(null);
                }
              }}
              onDrop={(e) => {
                if (validDropTarget) {
                  e.preventDefault();
                  drag.drop(entity.id);
                }
              }}
            >
              {expandable ? (
                <button
                  type="button"
                  aria-label={expanded ? "Collapse" : "Expand"}
                  className="flex size-4 flex-none items-center justify-center rounded hover:bg-foreground/10"
                  onClick={(e) => {
                    e.stopPropagation();
                    toggleExpanded(entity.id);
                  }}
                >
                  <ChevronRight
                    className={cn("size-3.5 transition-transform", expanded && "rotate-90")}
                  />
                </button>
              ) : (
                <span className="size-4 flex-none" />
              )}
              <span className="truncate">{entity.name}</span>
            </div>
          )}
        </ContextMenuTrigger>
        <ContextMenuContent className="min-w-36">
          <ContextMenuItem onSelect={() => actions.onFocus(entity.id)}>Focus</ContextMenuItem>
          <ContextMenuItem onSelect={() => actions.onRenameStart(entity.id)}>
            Rename
          </ContextMenuItem>
          <ContextMenuItem onSelect={() => actions.onCopy(entity.id)}>Copy</ContextMenuItem>
          <ContextMenuSeparator />
          <ContextMenuSub>
            <ContextMenuSubTrigger>Parent to…</ContextMenuSubTrigger>
            <ContextMenuSubContent className="max-h-64 min-w-36 overflow-y-auto">
              {parentTargets.length === 0 ? (
                <ContextMenuItem disabled>No valid parents</ContextMenuItem>
              ) : (
                parentTargets.map((target) => (
                  <ContextMenuItem
                    key={target.id}
                    onSelect={() => actions.onReparent(entity.id, target.id)}
                  >
                    {target.name}
                  </ContextMenuItem>
                ))
              )}
            </ContextMenuSubContent>
          </ContextMenuSub>
          {entity.parentId ? (
            <ContextMenuItem onSelect={() => actions.onReparent(entity.id, null)}>
              Unparent
            </ContextMenuItem>
          ) : null}
          <ContextMenuSeparator />
          <ContextMenuItem variant="destructive" onSelect={() => actions.onDelete(entity.id)}>
            Delete
          </ContextMenuItem>
        </ContextMenuContent>
      </ContextMenu>
      {hasChildren && expanded
        ? node.children.map((child) => (
            <TreeRow
              key={child.entity.id}
              node={child}
              depth={depth + 1}
              actions={actions}
              drag={drag}
            />
          ))
        : null}
      {expanded
        ? subrows.map((name) => (
            <SubRow key={`${entity.id}:${name}`} entity={entity} name={name} depth={depth + 1} />
          ))
        : null}
    </>
  );
}

/// A read-only component leaf under the selected entity: pure navigation, never an
/// edit surface and never a fetch. Clicking keeps the entity selected (local set
/// only — no control command), flips the bottom tab to the Inspector, and fires the
/// one-shot focus signal that scrolls the matching section into view.
function SubRow({ entity, name, depth }: { entity: EntityListEntry; name: string; depth: number }) {
  const selectEntity = useEditorStore((s) => s.selectEntity);
  const setBottomTab = useEditorStore((s) => s.setBottomTab);
  const setFocusComponent = useEditorStore((s) => s.setFocusComponent);
  return (
    <button
      type="button"
      role="treeitem"
      className="flex w-full items-center gap-0.5 rounded-md px-1 py-1 text-left text-xs italic text-muted-foreground hover:bg-accent"
      style={{ paddingLeft: depth * 14 + 20 }}
      onClick={() => {
        selectEntity(entity.id);
        setBottomTab("inspector");
        setFocusComponent(name);
      }}
    >
      {name}
    </button>
  );
}

/// Inline rename input rendered in place of a row. Autofocuses and selects all,
/// commits on Enter or blur, cancels on Escape. Enter and Escape unmount the input,
/// which fires a native blur; a `settled` ref ensures the blur does not commit a second
/// time after Enter and does not commit at all after Escape.
export function RenameRow({
  initial,
  onCommit,
  onCancel,
}: {
  initial: string;
  onCommit(next: string): void;
  onCancel(): void;
}) {
  const [value, setValue] = useState(initial);
  const settled = useRef(false);
  const inputRef = useRef<HTMLInputElement | null>(null);

  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      inputRef.current?.focus();
      inputRef.current?.select();
    });
    return () => cancelAnimationFrame(frame);
  }, []);

  return (
    <Input
      ref={inputRef}
      value={value}
      className="h-7 px-2.5 py-1.5 text-sm"
      onChange={(e) => setValue(e.target.value)}
      onFocus={(e) => e.currentTarget.select()}
      onBlur={() => {
        if (settled.current) {
          return;
        }
        settled.current = true;
        onCommit(value);
      }}
      onKeyDown={(e) => {
        if (e.key === "Enter") {
          e.preventDefault();
          if (settled.current) {
            return;
          }
          settled.current = true;
          onCommit(value);
        } else if (e.key === "Escape") {
          e.preventDefault();
          settled.current = true;
          onCancel();
        }
      }}
    />
  );
}
