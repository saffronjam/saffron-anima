/// The Hierarchy panel: a tree outliner over the scene entities, built client-side
/// from the flat `store.entities` slice plus each entry's `parentId` (refreshed by
/// the reconcile poll only when sceneVersion changes — this component never fetches).
/// A pinned Environment sentinel sits above the entity rows. Left-click selects
/// (optimistic + `select`); double-click renames inline (Enter commits via
/// `rename-entity`, Esc cancels); right-click opens Focus / Rename / Copy /
/// Parent to… / Unparent / Delete; dragging a row onto another reparents via
/// `set-parent`.
///
/// The context menu and the inline rename input are Radix/native controls anchored
/// on each row in the left column, and every drag affordance stays in the sidebar
/// DOM (the reparented X11 viewport paints over anything floating). Rejected control
/// calls surface in an inline flash at the bottom of the panel (no silent failures).
import { useState } from "react";
import { Bone, ListTree } from "lucide-react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { CreateMenu } from "../app/CreateMenu";
import { errorText, useFlash } from "../lib/flash";
import type { EntityListEntry } from "../protocol";
import { HierarchyTree, type TreeActions } from "./HierarchyTree";
import { Button } from "@/components/ui/button";
import { ScrollArea } from "@/components/ui/scroll-area";
import { cn } from "@/lib/utils";

export function HierarchyPanel() {
  const selectEntity = useEditorStore((s) => s.selectEntity);
  const setSelectedId = useEditorStore((s) => s.setSelectedId);
  const setParent = useEditorStore((s) => s.setParent);
  const applyOptimisticEntityName = useEditorStore((s) => s.applyOptimisticEntityName);
  const showComponentSubrows = useEditorStore((s) => s.showComponentSubrows);
  const toggleComponentSubrows = useEditorStore((s) => s.toggleComponentSubrows);
  const hideBones = useEditorStore((s) => s.hideBones);
  const toggleHideBones = useEditorStore((s) => s.toggleHideBones);
  const { message, flash } = useFlash();
  const [renamingId, setRenamingId] = useState<string | null>(null);

  const actions: TreeActions = {
    // Left-click a row: optimistic local select, then tell the engine. The poll
    // confirms via selectionVersion.
    onSelect: (entity: EntityListEntry): void => {
      selectEntity(entity.id);
      void client.selectEntity(entity.id).catch((err: unknown) => flash(errorText(err)));
    },
    // Aim the editor camera at the entity.
    onFocus: (id: string): void => {
      void client.focus(id).catch((err: unknown) => flash(errorText(err)));
    },
    // Copy duplicates the entity; the engine selects the dup, so mirror it locally
    // and let the sceneVersion bump refresh the list.
    onCopy: (id: string): void => {
      void client
        .copyEntity(id)
        .then((ref) => {
          selectEntity(ref.id);
        })
        .catch((err: unknown) => flash(errorText(err)));
    },
    // Delete removes the entity and its subtree; clear selection if it was selected.
    onDelete: (id: string): void => {
      if (useEditorStore.getState().selectedId === id) {
        setSelectedId(null);
      }
      void client.destroyEntity(id).catch((err: unknown) => flash(errorText(err)));
    },
    // Reparent (drag-drop or the context menu); the store action relinks
    // optimistically and rolls back on rejection — surface the error here.
    onReparent: (id: string, parentId: string | null): void => {
      void setParent(id, parentId).catch((err: unknown) => flash(errorText(err)));
    },
    renamingId,
    onRenameStart: (id: string): void => setRenamingId(id),
    // Inline rename: optimistically update the row name and commit via rename-entity;
    // the sceneVersion bump re-fetches the authoritative list. A rejection reverts to
    // the next poll's value and surfaces in the flash.
    onRenameCommit: (id: string, next: string): void => {
      setRenamingId(null);
      const trimmed = next.trim();
      if (trimmed === "") {
        return;
      }
      applyOptimisticEntityName(id, trimmed);
      void client.renameEntity(id, trimmed).catch((err: unknown) => flash(errorText(err)));
    },
    onRenameCancel: (): void => setRenamingId(null),
  };

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="flex h-10 flex-none items-center justify-between border-b border-border px-3">
        <span className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">
          Scene
        </span>
        <div className="flex items-center gap-1">
          <Button
            type="button"
            size="icon-xs"
            variant="ghost"
            aria-pressed={hideBones}
            title="Hide skeleton bones in the tree"
            className={cn(hideBones ? "bg-accent text-foreground" : "text-muted-foreground")}
            onClick={toggleHideBones}
          >
            <Bone />
          </Button>
          <Button
            type="button"
            size="icon-xs"
            variant="ghost"
            aria-pressed={showComponentSubrows}
            title="Show the selected entity's components as tree rows"
            className={cn(
              showComponentSubrows ? "bg-accent text-foreground" : "text-muted-foreground",
            )}
            onClick={toggleComponentSubrows}
          >
            <ListTree />
          </Button>
          <CreateMenu />
        </div>
      </div>
      <ScrollArea
        className="min-h-0 flex-1"
        onClick={(e) => {
          // A click in the empty space below/around the rows clears the selection,
          // mirroring the Escape shortcut. Row clicks land inside a treeitem.
          if ((e.target as Element).closest('[role="treeitem"]')) {
            return;
          }
          setSelectedId(null);
          void client.deselect().catch((err: unknown) => flash(errorText(err)));
        }}
      >
        <HierarchyTree actions={actions} />
      </ScrollArea>
      {message ? (
        <p className="flex-none border-t border-destructive/40 bg-destructive/10 px-2.5 py-1 text-[11px] text-destructive">
          {message}
        </p>
      ) : null}
    </div>
  );
}
