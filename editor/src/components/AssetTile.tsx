/// One asset catalog tile: a lazy thumbnail (mesh = its 3D render, texture = its
/// image, else a lucide type icon), an in-place rename input, and an HTML5 drag
/// SOURCE carrying `application/x-se-asset` (the React analog of the C++ `SE_ASSET`
/// / `AssetDragPayload`). Double-click opens the View modal; the Delete key on a
/// focused tile starts the same delete flow as the grid's shared context menu
/// (the panel owns that menu — a tile renders no Radix root of its own). Parity
/// target: `assetCatalogPanel` (editor_panels.cpp:226-325) + the `thumbnailFor`
/// fallbacks.
import { memo, useEffect, useRef, useState } from "react";
import { Box, File, Image as ImageIcon } from "lucide-react";
import { client } from "../control/client";
import { getCachedThumbnailUrl, getThumbnailUrl, useEditorStore } from "../state/store";
import { matchesBinding } from "../lib/keybindings";
import type { AssetEntry } from "../protocol";
import { cn } from "@/lib/utils";
import { useOutsideCommit } from "../lib/useOutsideCommit";
import { logRender } from "../lib/renderLog";
import { Input } from "@/components/ui/input";

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

/// The DnD payload for dragging catalog FOLDERS (their full paths) — distinct from
/// the asset payload so drop targets can route a folder move vs an asset move. A
/// drag may carry both mimes at once (a mixed folder + asset multi-selection).
export const FOLDER_DND_MIME = "application/x-se-folder";

/// The folder paths carried by a drag payload (empty if it is not a folder drag).
export function readFolderPayload(dt: DataTransfer): string[] {
  const raw = dt.getData(FOLDER_DND_MIME);
  if (!raw) {
    return [];
  }
  try {
    const parsed = JSON.parse(raw) as { path?: string; paths?: string[] };
    if (Array.isArray(parsed.paths)) {
      return parsed.paths.filter((p) => typeof p === "string" && p.length > 0);
    }
    if (typeof parsed.path === "string" && parsed.path.length > 0) {
      return [parsed.path];
    }
  } catch {
    // Malformed payload; treat as not-a-folder.
  }
  return [];
}

/// True when the drag in flight carries an asset or folder payload.
export function isCatalogDrag(dt: DataTransfer): boolean {
  return dt.types.includes(ASSET_DND_MIME) || dt.types.includes(FOLDER_DND_MIME);
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
  /// True while the panel has this tile in inline-rename mode (entered from the
  /// grid's shared context menu).
  renaming?: boolean;
  onView(entry: AssetEntry): void;
  onDelete(entry: AssetEntry): void;
  onSelect(entry: AssetEntry, event: React.MouseEvent): void;
  /// Write the drag payload (the panel owns selection, so it assembles the asset +
  /// folder ids the drag carries).
  onBeginDrag(entry: AssetEntry, event: React.DragEvent): void;
  /// The rename input settled (commit or cancel); the panel clears its rename state.
  onRenameEnd(): void;
}

/// Render the grid thumbnail at 128 px (parity with editor_app.cppm:138) and
/// display it at the 72-px tile size (parity with `tileSize`).
const THUMBNAIL_FETCH_SIZE = 128;

/// memo'd so a parent (grid) render skips tiles whose props are unchanged; the
/// callers keep every callback prop referentially stable for that reason.
export const AssetTile = memo(function AssetTile({
  entry,
  renaming = false,
  onView,
  onDelete,
  onSelect,
  onBeginDrag,
  onRenameEnd,
}: AssetTileProps) {
  logRender("AssetTile");
  // Membership-only subscription: the tile re-renders when ITS selected bit flips,
  // never on unrelated selection changes.
  const selected = useEditorStore((s) => s.selectedAssetIds.has(entry.id));
  const [url, setUrl] = useState<string | null>(() =>
    getCachedThumbnailUrl(entry.id, THUMBNAIL_FETCH_SIZE),
  );
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
  // (e.g. an `se rename-asset` reflected by the poll), but not while renaming.
  useEffect(() => {
    if (!renaming) {
      setDraft(entry.name);
    }
  }, [entry.name, renaming]);

  const commitRename = (): void => {
    onRenameEnd();
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

  return (
    <div className="relative w-[72px]">
      <div
        data-asset-tile-id={entry.id}
        data-asset-item="true"
        // Focusable so a click lands keyboard focus on the tile and the Delete
        // key can run the same delete flow as the context menu item.
        tabIndex={0}
        // Renaming disables the drag so a tile-drag never starts while typing.
        draggable={!renaming}
        onDragStart={(event) => onBeginDrag(entry, event)}
        onClick={(event) => onSelect(entry, event)}
        onDoubleClick={() => onView(entry)}
        onKeyDown={(event) => {
          if (
            !renaming &&
            matchesBinding(event, "assets.delete", useEditorStore.getState().keyBindings)
          ) {
            event.preventDefault();
            onDelete(entry);
          }
        }}
        className={cn(
          "group flex w-[72px] cursor-grab flex-col gap-1 rounded-md border border-transparent p-1",
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
        {renaming ? (
          <RenameInput
            value={draft}
            onChange={setDraft}
            onCommit={commitRename}
            onCancel={() => {
              onRenameEnd();
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
    </div>
  );
});

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
  useOutsideCommit(ref, onCommit);
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
