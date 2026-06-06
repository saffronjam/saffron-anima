/// One asset catalog tile: a lazy thumbnail (mesh = its 3D render, texture = its
/// image, else a lucide type icon), an in-place rename input, and an HTML5 drag
/// SOURCE carrying `application/x-se-asset` (the React analog of the C++ `SE_ASSET`
/// / `AssetDragPayload`). Double-click opens the View modal. Parity target:
/// `assetCatalogPanel` (editor_panels.cpp:226-325) + the `thumbnailFor` fallbacks.
import { useEffect, useRef, useState } from "react";
import { Box, Eye, File, Image as ImageIcon, Pencil, Trash } from "lucide-react";
import { client } from "../control/client";
import { getCachedThumbnailUrl, getThumbnailUrl, useEditorStore } from "../state/store";
import type { AssetEntry } from "../protocol";
import { cn } from "@/lib/utils";
import { Input } from "@/components/ui/input";
import {
  ContextMenu,
  ContextMenuContent,
  ContextMenuItem,
  ContextMenuSeparator,
  ContextMenuTrigger,
} from "@/components/ui/context-menu";

/// The DnD payload written to `application/x-se-asset` (distinct from an OS file
/// drop). `type` lets a drop target type-gate the accept (parity with the C++
/// `drag->type == type` guard, editor_components.cpp:77).
export const ASSET_DND_MIME = "application/x-se-asset";

export interface AssetDragPayload {
  id?: string;
  ids?: string[];
  type?: AssetEntry["type"];
}

/// Try to read an asset drag payload off a DataTransfer; null if it is not one.
export function readAssetPayload(dt: DataTransfer): AssetDragPayload | null {
  const raw = dt.getData(ASSET_DND_MIME);
  if (!raw) {
    return null;
  }
  try {
    const parsed = JSON.parse(raw) as Partial<AssetDragPayload>;
    if (Array.isArray(parsed.ids) && parsed.ids.every((id) => typeof id === "string")) {
      return { ids: parsed.ids };
    }
    if (typeof parsed.id === "string") {
      return { id: parsed.id, type: parsed.type };
    }
  } catch {
    // Malformed payload; treat as not-an-asset.
  }
  return null;
}

/// The asset ids carried by a drag payload, preferring the multi-select list.
export function assetIdsFromPayload(payload: AssetDragPayload | null): string[] {
  if (!payload) {
    return [];
  }
  if (payload.ids && payload.ids.length > 0) {
    return payload.ids;
  }
  return payload.id ? [payload.id] : [];
}

function TypeIcon({ type }: { type: AssetEntry["type"] }) {
  const className = "size-7 text-muted-foreground";
  if (type === "mesh") {
    return <Box className={className} />;
  }
  if (type === "texture") {
    return <ImageIcon className={className} />;
  }
  return <File className={className} />;
}

export interface AssetTileProps {
  entry: AssetEntry;
  selected?: boolean;
  onView(entry: AssetEntry): void;
  onDelete(entry: AssetEntry): void;
  onSelect(entry: AssetEntry, event: React.MouseEvent): void;
  getDragAssetIds(entry: AssetEntry): string[];
  confirmingDelete?: boolean;
  deleteBody?: string;
  onConfirmDelete?(entry: AssetEntry): void;
  onCancelDelete?(): void;
}

/// Render the grid thumbnail at 128 px (parity with editor_app.cppm:138) and
/// display it at the 72-px tile size (parity with `tileSize`).
const THUMBNAIL_FETCH_SIZE = 128;

export function AssetTile({
  entry,
  selected = false,
  onView,
  onDelete,
  onSelect,
  getDragAssetIds,
  confirmingDelete = false,
  deleteBody,
  onConfirmDelete,
  onCancelDelete,
}: AssetTileProps) {
  const [url, setUrl] = useState<string | null>(() =>
    getCachedThumbnailUrl(entry.id, THUMBNAIL_FETCH_SIZE),
  );
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(entry.name);
  const refreshAssets = useEditorStore((s) => s.refreshAssets);

  // Lazy thumbnail: fetch on mount / id change. The shared cache dedupes across
  // tiles; a rejection leaves `url` null so the type icon shows (parity with the
  // `*Icon.id` fallback in thumbnailFor).
  useEffect(() => {
    let cancelled = false;
    setUrl(getCachedThumbnailUrl(entry.id, THUMBNAIL_FETCH_SIZE));
    void getThumbnailUrl(entry.id, THUMBNAIL_FETCH_SIZE)
      .then((resolved) => {
        if (!cancelled) {
          setUrl(resolved);
        }
      })
      .catch(() => {
        // Keep the type-icon fallback.
      });
    return () => {
      cancelled = true;
    };
  }, [entry.id]);

  // Keep the rename draft in sync when the catalog name changes externally
  // (e.g. an `se rename-asset` reflected by the poll), but not while editing.
  useEffect(() => {
    if (!editing) {
      setDraft(entry.name);
    }
  }, [entry.name, editing]);

  const commitRename = (): void => {
    setEditing(false);
    const next = draft.trim();
    if (next.length === 0 || next === entry.name) {
      setDraft(entry.name);
      return;
    }
    void client
      .renameAsset(entry.id, next)
      .then(() => refreshAssets())
      .catch(() => setDraft(entry.name));
  };

  const onDragStart = (event: React.DragEvent<HTMLDivElement>): void => {
    const ids = getDragAssetIds(entry);
    const payload: AssetDragPayload =
      ids.length > 1 ? { ids } : { id: entry.id, ids: [entry.id], type: entry.type };
    event.dataTransfer.setData(ASSET_DND_MIME, JSON.stringify(payload));
    event.dataTransfer.effectAllowed = "copyMove";
  };

  const beginRename = (): void => {
    setDraft(entry.name);
    setEditing(true);
  };

  return (
    <div className="relative w-[72px]" onContextMenu={(event) => event.stopPropagation()}>
      <ContextMenu>
        <ContextMenuTrigger asChild>
          <div
            data-asset-tile-id={entry.id}
            data-asset-item="true"
            // Editing disables the drag so a tile-drag never starts while typing.
            draggable={!editing}
            onDragStart={onDragStart}
            onClick={(event) => onSelect(entry, event)}
            onDoubleClick={() => onView(entry)}
            className={cn(
              "group flex w-[72px] cursor-grab flex-col gap-1 rounded-md border border-border bg-background p-1",
              "transition-colors hover:border-ring hover:bg-accent/40 active:cursor-grabbing",
              selected && "border-ring bg-accent/60 ring-1 ring-ring",
            )}
          >
            <div className="flex aspect-square w-full items-center justify-center overflow-hidden rounded-sm bg-muted">
              {url ? (
                <img
                  src={url}
                  alt={entry.name}
                  className="size-full object-contain"
                  draggable={false}
                />
              ) : (
                <TypeIcon type={entry.type} />
              )}
            </div>
            {editing ? (
              <RenameInput
                value={draft}
                onChange={setDraft}
                onCommit={commitRename}
                onCancel={() => {
                  setEditing(false);
                  setDraft(entry.name);
                }}
              />
            ) : (
              <button
                type="button"
                className="truncate rounded-sm px-0.5 text-center text-[11px] leading-tight text-foreground"
              >
                {entry.name}
              </button>
            )}
          </div>
        </ContextMenuTrigger>
        <ContextMenuContent className="min-w-32">
          <ContextMenuItem onSelect={() => onView(entry)}>
            <Eye />
            View
          </ContextMenuItem>
          <ContextMenuItem onSelect={beginRename}>
            <Pencil />
            Rename
          </ContextMenuItem>
          <ContextMenuSeparator />
          <ContextMenuItem
            variant="destructive"
            className="bg-destructive/10 text-destructive focus:bg-destructive focus:text-destructive-foreground"
            onSelect={() => onDelete(entry)}
          >
            <Trash />
            Delete
          </ContextMenuItem>
        </ContextMenuContent>
      </ContextMenu>
      {confirmingDelete && onConfirmDelete && onCancelDelete ? (
        <DeleteConfirm
          title={`Delete ${entry.name}?`}
          body={deleteBody ?? "This removes the catalog entry and imported file."}
          onConfirm={() => onConfirmDelete(entry)}
          onCancel={onCancelDelete}
        />
      ) : null}
    </div>
  );
}

export function DeleteConfirm({
  title,
  body,
  onConfirm,
  onCancel,
}: {
  title: string;
  body: string;
  onConfirm(): void;
  onCancel(): void;
}) {
  return (
    <div
      className="absolute left-0 top-full z-40 mt-1 w-56 rounded-md border border-border bg-popover p-2 text-popover-foreground shadow-lg"
      onPointerDown={(event) => event.stopPropagation()}
      onClick={(event) => event.stopPropagation()}
    >
      <p className="text-xs font-medium">{title}</p>
      <p className="mt-1 text-[11px] leading-snug text-muted-foreground">{body}</p>
      <div className="mt-2 flex justify-end gap-1">
        <button
          type="button"
          className="rounded-sm px-2 py-1 text-[11px] text-muted-foreground hover:bg-accent hover:text-foreground"
          onClick={onCancel}
        >
          Cancel
        </button>
        <button
          type="button"
          className="rounded-sm bg-destructive px-2 py-1 text-[11px] text-destructive-foreground hover:bg-destructive/90"
          onClick={onConfirm}
        >
          Delete
        </button>
      </div>
    </div>
  );
}

interface RenameInputProps {
  value: string;
  onChange(value: string): void;
  onCommit(): void;
  onCancel(): void;
}

function RenameInput({ value, onChange, onCommit, onCancel }: RenameInputProps) {
  const ref = useRef<HTMLInputElement | null>(null);
  useEffect(() => {
    ref.current?.focus();
    ref.current?.select();
  }, []);
  return (
    <Input
      ref={ref}
      value={value}
      // Stop pointer/drag propagation so the tile drag never starts mid-edit.
      onPointerDown={(event) => event.stopPropagation()}
      onChange={(event) => onChange(event.currentTarget.value)}
      onBlur={onCommit}
      onKeyDown={(event) => {
        if (event.key === "Enter") {
          event.preventDefault();
          onCommit();
        } else if (event.key === "Escape") {
          event.preventDefault();
          onCancel();
        }
      }}
      className="h-5 rounded-sm px-1 py-0 text-center font-mono text-[11px]"
    />
  );
}
