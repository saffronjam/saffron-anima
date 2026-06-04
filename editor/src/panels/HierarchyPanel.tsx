/// The Hierarchy panel: a flat list of scene entities (parity with the C++
/// `hierarchyPanel`, which iterates `forEach<IdComponent, NameComponent>`). The
/// list comes from `store.entities`, refreshed by the reconcile poll only when
/// sceneVersion changes — this component never fetches; it just renders the
/// store slice. A left-click selects (optimistic + `select`); a right-click opens
/// a context menu offering Focus / Rename / Copy / Delete. A double-click on a row
/// starts an inline rename (Enter commits via `rename-entity`, Esc cancels).
///
/// The context menu and the inline rename input are Radix/native controls anchored
/// on each row in the left column. Rejected control calls surface in an inline flash
/// at the bottom of the panel (no silent failures).
import { useRef, useState } from "react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { CreateMenu } from "../app/CreateMenu";
import { errorText, useFlash } from "../lib/flash";
import type { EntityRef } from "../protocol";
import { Input } from "@/components/ui/input";
import { ScrollArea } from "@/components/ui/scroll-area";
import {
  ContextMenu,
  ContextMenuContent,
  ContextMenuItem,
  ContextMenuSeparator,
  ContextMenuTrigger,
} from "@/components/ui/context-menu";
import { cn } from "@/lib/utils";

export function HierarchyPanel() {
  const entities = useEditorStore((s) => s.entities);
  const selectedId = useEditorStore((s) => s.selectedId);
  const selectEntity = useEditorStore((s) => s.selectEntity);
  const setSelectedId = useEditorStore((s) => s.setSelectedId);
  const applyOptimisticEntityName = useEditorStore((s) => s.applyOptimisticEntityName);
  const { message, flash } = useFlash();
  const [renamingId, setRenamingId] = useState<string | null>(null);

  // Left-click a row: optimistic local select, then tell the engine. The poll
  // confirms via selectionVersion.
  const onSelect = (entity: EntityRef): void => {
    selectEntity(entity.id);
    void client.selectEntity(entity.id).catch((err: unknown) => flash(errorText(err)));
  };

  // Aim the editor camera at the entity.
  const onFocus = (id: string): void => {
    void client.focus(id).catch((err: unknown) => flash(errorText(err)));
  };

  // Copy duplicates the entity; the engine selects the dup, so mirror it locally
  // and let the sceneVersion bump refresh the list.
  const onCopy = (id: string): void => {
    void client
      .copyEntity(id)
      .then((ref) => {
        selectEntity(ref.id);
      })
      .catch((err: unknown) => flash(errorText(err)));
  };

  // Delete removes the entity; clear selection if it was the selected one.
  const onDelete = (id: string): void => {
    if (useEditorStore.getState().selectedId === id) {
      setSelectedId(null);
    }
    void client.destroyEntity(id).catch((err: unknown) => flash(errorText(err)));
  };

  // Inline rename: optimistically update the row name and commit via rename-entity;
  // the sceneVersion bump re-fetches the authoritative list. A rejection reverts to
  // the next poll's value and surfaces in the flash.
  const commitRename = (id: string, next: string): void => {
    setRenamingId(null);
    const trimmed = next.trim();
    if (trimmed === "") {
      return;
    }
    applyOptimisticEntityName(id, trimmed);
    void client.renameEntity(id, trimmed).catch((err: unknown) => flash(errorText(err)));
  };

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="flex h-10 flex-none items-center justify-between border-b border-border px-3">
        <span className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">
          Scene
        </span>
        <CreateMenu />
      </div>
      <ScrollArea className="min-h-0 flex-1">
        <div className="p-1" role="listbox" aria-label="Scene entities">
          {entities.length === 0 ? (
            <div className="p-2.5 text-center italic text-muted-foreground">No entities</div>
          ) : (
            entities.map((entity) => (
              <ContextMenu key={entity.id}>
                <ContextMenuTrigger asChild>
                  {entity.id === renamingId ? (
                    <RenameRow
                      initial={entity.name}
                      onCommit={(next) => commitRename(entity.id, next)}
                      onCancel={() => setRenamingId(null)}
                    />
                  ) : (
                    <button
                      type="button"
                      role="option"
                      aria-selected={entity.id === selectedId}
                      className={cn(
                        "block w-full truncate rounded-md px-2.5 py-1.5 text-left text-sm",
                        entity.id === selectedId
                          ? "bg-primary text-primary-foreground"
                          : "text-foreground hover:bg-accent",
                      )}
                      onClick={() => onSelect(entity)}
                      onDoubleClick={() => setRenamingId(entity.id)}
                    >
                      {entity.name}
                    </button>
                  )}
                </ContextMenuTrigger>
                <ContextMenuContent className="min-w-36">
                  <ContextMenuItem onSelect={() => onFocus(entity.id)}>Focus</ContextMenuItem>
                  <ContextMenuItem onSelect={() => setRenamingId(entity.id)}>
                    Rename
                  </ContextMenuItem>
                  <ContextMenuItem onSelect={() => onCopy(entity.id)}>Copy</ContextMenuItem>
                  <ContextMenuSeparator />
                  <ContextMenuItem variant="destructive" onSelect={() => onDelete(entity.id)}>
                    Delete
                  </ContextMenuItem>
                </ContextMenuContent>
              </ContextMenu>
            ))
          )}
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

/// Inline rename input rendered in place of a row. Autofocuses and selects all,
/// commits on Enter or blur, cancels on Escape. Enter and Escape unmount the input,
/// which fires a native blur; a `settled` ref ensures the blur does not commit a second
/// time after Enter and does not commit at all after Escape.
function RenameRow({
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
  return (
    <Input
      autoFocus
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
