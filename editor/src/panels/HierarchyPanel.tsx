/// The Hierarchy panel: a flat list of scene entities (parity with the C++
/// `hierarchyPanel`, which iterates `forEach<IdComponent, NameComponent>`). The
/// list comes from `store.entities`, refreshed by the reconcile poll only when
/// sceneVersion changes — this component never fetches; it just renders the
/// store slice. A left-click selects (optimistic + `select`); a right-click
/// opens a context menu offering Copy / Delete.
///
/// The context menu is a Radix ContextMenu anchored to each row. The Hierarchy
/// lives in the left column and Radix positions the content at the cursor (over
/// the sidebar, never over the reparented native viewport rect). No inline
/// rename — rename is the Inspector's Name field, matching the C++ editor.
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { CreateMenu } from "../app/CreateMenu";
import type { EntityRef } from "../protocol";
import { ScrollArea } from "@/components/ui/scroll-area";
import {
  ContextMenu,
  ContextMenuContent,
  ContextMenuItem,
  ContextMenuTrigger,
} from "@/components/ui/context-menu";
import { cn } from "@/lib/utils";

export function HierarchyPanel() {
  const entities = useEditorStore((s) => s.entities);
  const selectedId = useEditorStore((s) => s.selectedId);
  const selectEntity = useEditorStore((s) => s.selectEntity);
  const setSelectedId = useEditorStore((s) => s.setSelectedId);

  // Left-click a row: optimistic local select, then tell the engine. The poll
  // confirms via selectionVersion.
  const onSelect = (entity: EntityRef): void => {
    selectEntity(entity.id);
    void client.selectEntity(entity.id).catch(() => {});
  };

  // Copy duplicates the entity; the engine selects the dup, so mirror it locally
  // and let the sceneVersion bump refresh the list.
  const onCopy = (id: string): void => {
    void client
      .copyEntity(id)
      .then((ref) => {
        selectEntity(ref.id);
      })
      .catch(() => {});
  };

  // Delete removes the entity; clear selection if it was the selected one.
  const onDelete = (id: string): void => {
    if (useEditorStore.getState().selectedId === id) {
      setSelectedId(null);
    }
    void client.destroyEntity(id).catch(() => {});
  };

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="flex h-10 flex-none items-center justify-between border-b border-border px-3">
        <span className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">
          Hierarchy
        </span>
        <CreateMenu />
      </div>
      <ScrollArea className="min-h-0 flex-1">
        <div className="p-1" role="listbox" aria-label="Scene entities">
          {entities.length === 0 ? (
            <div className="p-2.5 text-center italic text-muted-foreground">
              No entities
            </div>
          ) : (
            entities.map((entity) => (
              <ContextMenu key={entity.id}>
                <ContextMenuTrigger asChild>
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
                  >
                    {entity.name}
                  </button>
                </ContextMenuTrigger>
                <ContextMenuContent className="min-w-36">
                  <ContextMenuItem onSelect={() => onCopy(entity.id)}>
                    Copy
                  </ContextMenuItem>
                  <ContextMenuItem
                    variant="destructive"
                    onSelect={() => onDelete(entity.id)}
                  >
                    Delete
                  </ContextMenuItem>
                </ContextMenuContent>
              </ContextMenu>
            ))
          )}
        </div>
      </ScrollArea>
    </div>
  );
}
