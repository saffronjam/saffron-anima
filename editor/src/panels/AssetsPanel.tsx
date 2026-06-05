/// The Assets panel: the React port of the C++ `assetCatalogPanel`
/// (editor_panels.cpp:226-325). A responsive tile grid over `store.assets`, an
/// Import button (Tauri file dialog), an OS file-drop target, and the View modal.
/// Imports route by extension (parity with `importToCatalog`,
/// editor_app.cppm:188-205): images → import-texture (no spawn), everything else →
/// import-model (spawns + selects an entity, matching `se import-model`).
import { useCallback, useEffect, useRef, useState } from "react";
import type { MouseEvent as ReactMouseEvent, PointerEvent as ReactPointerEvent } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import { ArrowUp, Folder, FolderPlus, Pen, Plus, Trash } from "lucide-react";
import { client } from "../control/client";
import { invalidateThumbnails, useEditorStore, withNativeDialog } from "../state/store";
import { AssetTile, DeleteConfirm } from "../components/AssetTile";
import { ASSET_DND_MIME, readAssetPayload } from "../components/AssetTile";
import type { AssetEntry } from "../protocol";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { ScrollArea } from "@/components/ui/scroll-area";
import { cn } from "@/lib/utils";
import {
  ContextMenu,
  ContextMenuContent,
  ContextMenuItem,
  ContextMenuSeparator,
  ContextMenuTrigger,
} from "@/components/ui/context-menu";

/// Image extensions that import as a catalog texture; everything else is imported
/// as a model (parity with editor_app.cppm:201).
const TEXTURE_EXTS = new Set(["png", "jpg", "jpeg", "hdr", "tga", "bmp"]);

/// Model + image extensions offered in the file dialog.
const MODEL_EXTS = ["gltf", "glb", "obj", "smesh"];
const IMAGE_EXTS = ["png", "jpg", "jpeg", "hdr", "tga", "bmp"];

function extensionOf(path: string): string {
  const dot = path.lastIndexOf(".");
  return dot >= 0 ? path.slice(dot + 1).toLowerCase() : "";
}

function parentFolderPath(folder: string): string | null {
  const slash = folder.lastIndexOf("/");
  return slash >= 0 ? folder.slice(0, slash) : null;
}

function folderLabel(folder: string): string {
  const slash = folder.lastIndexOf("/");
  return slash >= 0 ? folder.slice(slash + 1) : folder;
}

function isFolderDescendant(candidate: string, folder: string): boolean {
  return (
    candidate.length > folder.length &&
    candidate.startsWith(folder) &&
    candidate[folder.length] === "/"
  );
}

function replaceFolderPrefix(value: string, from: string, to: string): string {
  if (value === from) {
    return to;
  }
  return isFolderDescendant(value, from) ? `${to}${value.slice(from.length)}` : value;
}

function normalizeFolderInput(input: string, parent: string | null): string | null {
  const trimmed = input.trim().replaceAll("\\", "/");
  if (!trimmed) {
    return null;
  }
  const segments = trimmed.split("/");
  if (segments.some((segment) => segment.trim().length === 0)) {
    return null;
  }
  const relative = segments.map((segment) => segment.trim()).join("/");
  return parent ? `${parent}/${relative}` : relative;
}

function folderAncestorPaths(folder: string): string[] {
  const segments = folder.split("/");
  const paths: string[] = [];
  for (let i = 1; i <= segments.length; i += 1) {
    paths.push(segments.slice(0, i).join("/"));
  }
  return paths;
}

async function importPath(path: string, folder: string | null): Promise<void> {
  try {
    if (TEXTURE_EXTS.has(extensionOf(path))) {
      const imported = await client.importTexture(path);
      if (folder) {
        await client.moveAsset(imported.texture, folder);
      }
    } else {
      const imported = await client.importModel(path);
      if (folder) {
        await moveImportedAsset(imported.mesh, folder);
        await moveImportedAsset(imported.albedoTexture, folder);
      }
    }
  } catch {
    // The engine reports a bad import as ok:false (a rejected promise); swallow so
    // a single bad file doesn't break a multi-file drop.
  }
}

export function AssetsPanel() {
  const assets = useEditorStore((s) => s.assets);
  const folders = useEditorStore((s) => s.assetFolders);
  const refreshAssets = useEditorStore((s) => s.refreshAssets);
  const nativeDialogOpen = useEditorStore((s) => s.nativeDialogOpen);
  const openAssetTab = useEditorStore((s) => s.openAssetTab);
  const closeViewTab = useEditorStore((s) => s.closeViewTab);
  const [currentFolder, setCurrentFolder] = useState<string | null>(null);
  const [creatingFolder, setCreatingFolder] = useState(false);
  const [creatingFolderName, setCreatingFolderName] = useState("");
  const [renamingFolder, setRenamingFolder] = useState<string | null>(null);
  const [pendingAssetDelete, setPendingAssetDelete] = useState<PendingAssetDelete | null>(null);
  const [pendingFolderDelete, setPendingFolderDelete] = useState<string | null>(null);
  const [selectedAssetIds, setSelectedAssetIds] = useState<Set<string>>(() => new Set());
  const [selectionAnchorId, setSelectionAnchorId] = useState<string | null>(null);
  const [folderError, setFolderError] = useState<string | null>(null);
  const [dropActive, setDropActive] = useState(false);
  const [assetDropTarget, setAssetDropTarget] = useState<string | null>(null);
  const visibleAssets = assets.filter((asset) => (asset.folder ?? "") === (currentFolder ?? ""));

  useEffect(() => {
    if (currentFolder && !folders.includes(currentFolder)) {
      setCurrentFolder(null);
    }
  }, [currentFolder, folders]);

  useEffect(() => {
    const visibleIds = new Set(visibleAssets.map((asset) => asset.id));
    setSelectedAssetIds((current) => {
      const next = new Set([...current].filter((id) => visibleIds.has(id)));
      return next.size === current.size ? current : next;
    });
    setSelectionAnchorId((current) => (current && visibleIds.has(current) ? current : null));
  }, [visibleAssets]);

  const importMany = useCallback(
    async (paths: string[]): Promise<void> => {
      for (const path of paths) {
        await importPath(path, currentFolder);
      }
      await refreshAssets();
    },
    [currentFolder, refreshAssets],
  );

  // OS file-drop: Tauri delivers native file drops via the webview drag-drop event
  // (a distinct channel from the HTML5 `application/x-se-asset` tile DnD). We only
  // import when the drop position is inside this panel's rect, so dropping a model
  // on the viewport doesn't trigger a catalog import here.
  useEffect(() => {
    let unlisten: (() => void) | null = null;
    let disposed = false;
    void getCurrentWebview()
      .onDragDropEvent((event) => {
        const payload = event.payload;
        if (payload.type === "enter" || payload.type === "over") {
          setDropActive(isInsidePanel(payload.position));
        } else if (payload.type === "leave") {
          setDropActive(false);
        } else if (payload.type === "drop") {
          setDropActive(false);
          if (payload.paths.length > 0 && isInsidePanel(payload.position)) {
            void importMany(payload.paths);
          }
        }
      })
      .then((fn) => {
        if (disposed) {
          fn();
        } else {
          unlisten = fn;
        }
      });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [importMany]);

  const onImportClick = useCallback(async (): Promise<void> => {
    const selection = await withNativeDialog(() =>
      open({
        multiple: true,
        filters: [
          { name: "Models & Images", extensions: [...MODEL_EXTS, ...IMAGE_EXTS] },
          { name: "Models", extensions: MODEL_EXTS },
          { name: "Images", extensions: IMAGE_EXTS },
        ],
      }),
    );
    if (!selection) {
      return;
    }
    await importMany(Array.isArray(selection) ? selection : [selection]);
  }, [importMany]);

  const onNewFolder = useCallback((): void => {
    setFolderError(null);
    setCreatingFolderName("");
    setCreatingFolder(true);
  }, []);

  const commitNewFolder = useCallback(
    async (name: string): Promise<void> => {
      const folder = normalizeFolderInput(name, currentFolder);
      if (!folder) {
        setCreatingFolder(false);
        setCreatingFolderName("");
        setFolderError(null);
        return;
      }
      if (folders.includes(folder)) {
        setFolderError("Folder already exists");
        setCreatingFolder(true);
        return;
      }
      try {
        for (const path of folderAncestorPaths(folder)) {
          if (!folders.includes(path)) {
            await client.createAssetFolder(path);
          }
        }
        await refreshAssets();
        setCreatingFolder(false);
        setCreatingFolderName("");
        setFolderError(null);
      } catch {
        setFolderError("Could not create folder");
        setCreatingFolder(true);
      }
    },
    [currentFolder, folders, refreshAssets],
  );

  const cancelNewFolder = useCallback((): void => {
    setCreatingFolder(false);
    setCreatingFolderName("");
    setFolderError(null);
  }, []);

  const commitRenameFolder = useCallback(
    async (folder: string, name: string): Promise<void> => {
      const next = normalizeFolderInput(name, parentFolderPath(folder));
      if (!next || next === folder) {
        setRenamingFolder(null);
        setFolderError(null);
        return;
      }
      if (folders.includes(next)) {
        setFolderError("Folder already exists");
        setRenamingFolder(folder);
        return;
      }
      try {
        const ancestors = folderAncestorPaths(next).slice(0, -1);
        for (const path of ancestors) {
          if (!folders.includes(path)) {
            await client.createAssetFolder(path);
          }
        }
        await client.renameAssetFolder(folder, next);
        await refreshAssets();
        if (currentFolder) {
          setCurrentFolder(replaceFolderPrefix(currentFolder, folder, next));
        }
        setRenamingFolder(null);
        setFolderError(null);
      } catch {
        setFolderError("Could not rename folder");
        setRenamingFolder(folder);
      }
    },
    [currentFolder, folders, refreshAssets],
  );

  const cancelRenameFolder = useCallback((): void => {
    setRenamingFolder(null);
    setFolderError(null);
  }, []);

  const moveAssetsToFolder = useCallback(
    async (assetIds: string[], folder: string | null): Promise<void> => {
      if (assetIds.length === 0) {
        return;
      }
      await Promise.all(assetIds.map((assetId) => client.moveAsset(assetId, folder)));
      await refreshAssets();
    },
    [refreshAssets],
  );

  const selectAsset = useCallback(
    (asset: AssetEntry, event: ReactMouseEvent): void => {
      const index = visibleAssets.findIndex((candidate) => candidate.id === asset.id);
      if (index < 0) {
        return;
      }
      if (event.shiftKey && selectionAnchorId) {
        const anchorIndex = visibleAssets.findIndex(
          (candidate) => candidate.id === selectionAnchorId,
        );
        if (anchorIndex >= 0) {
          const first = Math.min(anchorIndex, index);
          const last = Math.max(anchorIndex, index);
          setSelectedAssetIds((current) => {
            const next = new Set(current);
            for (const ranged of visibleAssets.slice(first, last + 1)) {
              next.add(ranged.id);
            }
            return next;
          });
          setSelectionAnchorId(asset.id);
          return;
        }
      }
      if (event.ctrlKey || event.metaKey) {
        setSelectedAssetIds((current) => {
          const next = new Set(current);
          if (next.has(asset.id)) {
            next.delete(asset.id);
          } else {
            next.add(asset.id);
          }
          return next;
        });
        setSelectionAnchorId(asset.id);
        return;
      }
      setSelectedAssetIds(new Set([asset.id]));
      setSelectionAnchorId(asset.id);
    },
    [selectionAnchorId, visibleAssets],
  );

  const getDragAssetIds = useCallback(
    (asset: AssetEntry): string[] => {
      if (!selectedAssetIds.has(asset.id)) {
        setSelectedAssetIds(new Set([asset.id]));
        setSelectionAnchorId(asset.id);
        return [asset.id];
      }
      const dragged = visibleAssets
        .map((candidate) => candidate.id)
        .filter((id) => selectedAssetIds.has(id));
      return dragged.length > 0 ? dragged : [asset.id];
    },
    [selectedAssetIds, visibleAssets],
  );

  const requestDeleteAsset = useCallback(async (asset: AssetEntry): Promise<void> => {
    setPendingFolderDelete(null);
    const usages = await client.assetUsages(asset.id);
    const usageLines = usages.usages.map((usage) => {
      if (usage.entityName) {
        return `${usage.entityName}: ${usage.slot}`;
      }
      return usage.slot;
    });
    setPendingAssetDelete({
      asset,
      body:
        usageLines.length > 0
          ? `Clears ${usageLines.length} usage${usageLines.length === 1 ? "" : "s"}: ${usageLines.join(", ")}.`
          : "Removes the catalog entry and imported file.",
    });
  }, []);

  const confirmDeleteAsset = useCallback(
    async (asset: AssetEntry): Promise<void> => {
      await client.deleteAsset(asset.id);
      setPendingAssetDelete(null);
      closeViewTab(`asset:${asset.id}`);
      invalidateThumbnails();
      await refreshAssets();
    },
    [closeViewTab, refreshAssets],
  );

  const requestDeleteFolder = useCallback((folder: string): void => {
    setPendingAssetDelete(null);
    setPendingFolderDelete(folder);
  }, []);

  const confirmDeleteFolder = useCallback(
    async (folder: string): Promise<void> => {
      await client.deleteAssetFolder(folder);
      if (
        currentFolder === folder ||
        (currentFolder && isFolderDescendant(currentFolder, folder))
      ) {
        setCurrentFolder(null);
      }
      setPendingFolderDelete(null);
      await refreshAssets();
    },
    [currentFolder, refreshAssets],
  );

  return (
    <div className="flex h-full min-h-0 flex-col" data-asset-panel="true">
      <div className="flex h-10 flex-none items-center justify-between border-b border-border px-3">
        <span className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">
          Assets
        </span>
        <div className="flex items-center gap-2">
          <Button
            type="button"
            size="sm"
            variant="outline"
            className="gap-1"
            onClick={onNewFolder}
            title="New folder"
          >
            <FolderPlus />
            New Folder
          </Button>
          <Button
            type="button"
            size="sm"
            variant="outline"
            className="gap-1"
            onClick={() => void onImportClick()}
            disabled={nativeDialogOpen}
            title="Import a model or texture"
          >
            <Plus />
            Import
          </Button>
        </div>
      </div>
      <ContextMenu>
        <ContextMenuTrigger asChild>
          <div className="min-h-0 flex-1">
            <ScrollArea className="h-full">
              <AssetPanelBody
                assets={visibleAssets}
                folders={folders}
                currentFolder={currentFolder}
                dropActive={dropActive}
                creatingFolder={creatingFolder}
                creatingFolderName={creatingFolderName}
                renamingFolder={renamingFolder}
                folderError={folderError}
                pendingAssetDelete={pendingAssetDelete}
                pendingFolderDelete={pendingFolderDelete}
                selectedAssetIds={selectedAssetIds}
                assetDropTarget={assetDropTarget}
                onOpenFolder={setCurrentFolder}
                onView={openAssetTab}
                onSelectAsset={selectAsset}
                onDeleteAsset={(asset) => void requestDeleteAsset(asset)}
                onConfirmDeleteAsset={(asset) => void confirmDeleteAsset(asset)}
                onCancelDeleteAsset={() => setPendingAssetDelete(null)}
                onDeleteFolder={requestDeleteFolder}
                onConfirmDeleteFolder={(folder) => void confirmDeleteFolder(folder)}
                onCancelDeleteFolder={() => setPendingFolderDelete(null)}
                onMoveAssets={(assetIds, folder) => void moveAssetsToFolder(assetIds, folder)}
                getDragAssetIds={getDragAssetIds}
                onSetMarqueeSelection={(assetIds) => {
                  setSelectedAssetIds(new Set(assetIds));
                  setSelectionAnchorId(assetIds.at(-1) ?? null);
                }}
                onCommitNewFolder={(name) => void commitNewFolder(name)}
                onChangeNewFolderName={setCreatingFolderName}
                onCancelNewFolder={cancelNewFolder}
                onStartRenameFolder={setRenamingFolder}
                onCommitRenameFolder={(folder, name) => void commitRenameFolder(folder, name)}
                onCancelRenameFolder={cancelRenameFolder}
                onAssetDropTarget={setAssetDropTarget}
                onClearFolderError={() => setFolderError(null)}
              />
            </ScrollArea>
          </div>
        </ContextMenuTrigger>
        <ContextMenuContent className="min-w-40">
          <ContextMenuItem onSelect={onNewFolder}>
            <Folder />
            New Folder
          </ContextMenuItem>
          <ContextMenuItem onSelect={() => void onImportClick()} disabled={nativeDialogOpen}>
            <Plus />
            Import
          </ContextMenuItem>
        </ContextMenuContent>
      </ContextMenu>
    </div>
  );
}

function AssetPanelBody({
  assets,
  folders,
  currentFolder,
  dropActive,
  creatingFolder,
  creatingFolderName,
  renamingFolder,
  folderError,
  pendingAssetDelete,
  pendingFolderDelete,
  selectedAssetIds,
  assetDropTarget,
  onOpenFolder,
  onView,
  onSelectAsset,
  onDeleteAsset,
  onConfirmDeleteAsset,
  onCancelDeleteAsset,
  onDeleteFolder,
  onConfirmDeleteFolder,
  onCancelDeleteFolder,
  onMoveAssets,
  getDragAssetIds,
  onSetMarqueeSelection,
  onCommitNewFolder,
  onChangeNewFolderName,
  onCancelNewFolder,
  onStartRenameFolder,
  onCommitRenameFolder,
  onCancelRenameFolder,
  onAssetDropTarget,
  onClearFolderError,
}: {
  assets: AssetEntry[];
  folders: string[];
  currentFolder: string | null;
  dropActive: boolean;
  creatingFolder: boolean;
  creatingFolderName: string;
  renamingFolder: string | null;
  folderError: string | null;
  pendingAssetDelete: PendingAssetDelete | null;
  pendingFolderDelete: string | null;
  selectedAssetIds: Set<string>;
  assetDropTarget: string | null;
  onOpenFolder(folder: string | null): void;
  onView(asset: AssetEntry): void;
  onSelectAsset(asset: AssetEntry, event: ReactMouseEvent): void;
  onDeleteAsset(asset: AssetEntry): void;
  onConfirmDeleteAsset(asset: AssetEntry): void;
  onCancelDeleteAsset(): void;
  onDeleteFolder(folder: string): void;
  onConfirmDeleteFolder(folder: string): void;
  onCancelDeleteFolder(): void;
  onMoveAssets(assetIds: string[], folder: string | null): void;
  getDragAssetIds(asset: AssetEntry): string[];
  onSetMarqueeSelection(assetIds: string[]): void;
  onCommitNewFolder(name: string): void;
  onChangeNewFolderName(name: string): void;
  onCancelNewFolder(): void;
  onStartRenameFolder(folder: string): void;
  onCommitRenameFolder(folder: string, name: string): void;
  onCancelRenameFolder(): void;
  onAssetDropTarget(folder: string | null): void;
  onClearFolderError(): void;
}) {
  const folderItems = sortedFolderItems(folders, currentFolder, creatingFolder);
  const empty =
    !creatingFolder && folderItems.length === 0 && assets.length === 0 && !currentFolder;
  const panelRef = useRef<HTMLDivElement | null>(null);
  const [marquee, setMarquee] = useState<MarqueeState | null>(null);

  const selectMarqueeAssets = (next: MarqueeState): void => {
    const panel = panelRef.current;
    if (!panel) {
      return;
    }
    const rect = marqueeRect(next);
    const selected = assets.filter((asset) => {
      const tile = panel.querySelector(`[data-asset-tile-id="${cssEscape(asset.id)}"]`);
      return tile instanceof HTMLElement && rectsIntersect(rect, tile.getBoundingClientRect());
    });
    onSetMarqueeSelection(selected.map((asset) => asset.id));
  };

  const startMarquee = (event: ReactPointerEvent<HTMLDivElement>): void => {
    if (event.button !== 0) {
      return;
    }
    const target = event.target;
    if (
      target instanceof Element &&
      target.closest("[data-asset-item='true'], [data-asset-folder='true'], button, input")
    ) {
      return;
    }
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    const next: MarqueeState = {
      startX: event.clientX,
      startY: event.clientY,
      currentX: event.clientX,
      currentY: event.clientY,
    };
    setMarquee(next);
    onSetMarqueeSelection([]);
  };

  const moveMarquee = (event: ReactPointerEvent<HTMLDivElement>): void => {
    if (!marquee) {
      return;
    }
    const next = { ...marquee, currentX: event.clientX, currentY: event.clientY };
    setMarquee(next);
    selectMarqueeAssets(next);
  };

  const endMarquee = (event: ReactPointerEvent<HTMLDivElement>): void => {
    if (!marquee) {
      return;
    }
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
    setMarquee(null);
  };

  return (
    <div
      ref={panelRef}
      className={
        dropActive
          ? "relative min-h-full rounded-sm p-2 ring-2 ring-inset ring-ring"
          : "relative min-h-full p-2"
      }
      onPointerDown={startMarquee}
      onPointerMove={moveMarquee}
      onPointerUp={endMarquee}
      onPointerCancel={endMarquee}
      onDragOver={(event) => {
        if (event.dataTransfer.types.includes(ASSET_DND_MIME)) {
          event.preventDefault();
          event.dataTransfer.dropEffect = "move";
          const target = event.target;
          if (!(target instanceof Element) || !target.closest("[data-asset-folder='true']")) {
            onAssetDropTarget(null);
          }
        }
      }}
      onDragLeave={(event) => {
        if (
          event.relatedTarget instanceof Node &&
          event.currentTarget.contains(event.relatedTarget)
        ) {
          return;
        }
        onAssetDropTarget(null);
      }}
      onDrop={(event) => {
        const payload = readAssetPayload(event.dataTransfer);
        if (!payload) {
          return;
        }
        event.preventDefault();
        onAssetDropTarget(null);
        onMoveAssets(assetIdsFromPayload(payload), currentFolder);
      }}
    >
      <div className="mb-2 flex h-7 items-center gap-1 text-xs text-muted-foreground">
        <Button type="button" size="xs" variant="ghost" onClick={() => onOpenFolder(null)}>
          Root
        </Button>
        {currentFolder ? (
          <>
            <span>/</span>
            <span className="font-mono text-foreground">{currentFolder}</span>
          </>
        ) : null}
      </div>
      {empty ? (
        <p className="px-1 py-3 text-center text-xs italic text-muted-foreground">
          No assets yet. Import or drag-and-drop a model or texture.
        </p>
      ) : (
        <div
          className="grid gap-2"
          style={{ gridTemplateColumns: "repeat(auto-fill, minmax(72px, 1fr))" }}
        >
          {currentFolder ? (
            <FolderTile
              name="Root"
              kind="root"
              dragActive={assetDropTarget === ""}
              onOpen={() => onOpenFolder(null)}
              onAssetDropTarget={() => onAssetDropTarget("")}
              onClearAssetDropTarget={() => onAssetDropTarget(null)}
              onMoveAssets={(assetIds) => onMoveAssets(assetIds, null)}
            />
          ) : null}
          {folderItems.map((item) =>
            item.kind === "new" ? (
              <NewFolderTile
                key="new-folder"
                value={creatingFolderName}
                invalid={folderError !== null}
                onChange={(name) => {
                  onChangeNewFolderName(name);
                  if (folderError !== null) {
                    onClearFolderError();
                  }
                }}
                onCommit={onCommitNewFolder}
                onCancel={onCancelNewFolder}
              />
            ) : (
              <FolderTile
                key={item.path}
                name={item.label}
                editing={renamingFolder === item.path}
                invalid={folderError !== null && renamingFolder === item.path}
                dragActive={assetDropTarget === item.path}
                onOpen={() => onOpenFolder(item.path)}
                onAssetDropTarget={() => onAssetDropTarget(item.path)}
                onClearAssetDropTarget={() => onAssetDropTarget(null)}
                onMoveAssets={(assetIds) => onMoveAssets(assetIds, item.path)}
                onRename={() => onStartRenameFolder(item.path)}
                onDelete={() => onDeleteFolder(item.path)}
                confirmingDelete={pendingFolderDelete === item.path}
                deleteBody={deleteFolderBody(item.path)}
                onConfirmDelete={() => onConfirmDeleteFolder(item.path)}
                onCancelDelete={onCancelDeleteFolder}
                onCommitRename={(name) => onCommitRenameFolder(item.path, name)}
                onChangeRename={onClearFolderError}
                onCancelRename={onCancelRenameFolder}
              />
            ),
          )}
          {assets.map((asset) => (
            <AssetTile
              key={asset.id}
              entry={asset}
              selected={selectedAssetIds.has(asset.id)}
              onView={onView}
              onDelete={onDeleteAsset}
              onSelect={onSelectAsset}
              getDragAssetIds={getDragAssetIds}
              confirmingDelete={pendingAssetDelete?.asset.id === asset.id}
              deleteBody={pendingAssetDelete?.body}
              onConfirmDelete={onConfirmDeleteAsset}
              onCancelDelete={onCancelDeleteAsset}
            />
          ))}
        </div>
      )}
      {marquee ? <MarqueeBox panel={panelRef.current} marquee={marquee} /> : null}
    </div>
  );

  function assetsInFolderCount(folder: string): number {
    return useEditorStore
      .getState()
      .assets.filter(
        (asset) => asset.folder === folder || isFolderDescendant(asset.folder ?? "", folder),
      ).length;
  }

  function deleteFolderBody(folder: string): string {
    const count = assetsInFolderCount(folder);
    return `Moves ${count} asset${count === 1 ? "" : "s"} to Root.`;
  }
}

function NewFolderTile({
  value,
  invalid = false,
  onChange,
  onCommit,
  onCancel,
}: {
  value: string;
  invalid?: boolean;
  onChange(name: string): void;
  onCommit(name: string): void;
  onCancel(): void;
}) {
  const inputRef = useRef<HTMLInputElement | null>(null);
  const settledRef = useRef(false);

  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      inputRef.current?.focus();
      inputRef.current?.select();
    });
    return () => cancelAnimationFrame(frame);
  }, []);

  const commit = (): void => {
    if (settledRef.current) {
      return;
    }
    settledRef.current = true;
    onCommit(value);
    window.setTimeout(() => {
      settledRef.current = false;
    }, 100);
  };

  return (
    <div
      className="flex w-[72px] flex-col gap-1 rounded-md border border-ring bg-background p-1"
      data-asset-folder="true"
    >
      <div className="flex aspect-square w-full items-center justify-center rounded-sm bg-muted">
        <Folder className="size-8 text-muted-foreground" />
      </div>
      <Input
        ref={inputRef}
        value={value}
        aria-invalid={invalid}
        className={cn(
          "h-5 rounded-sm px-1 py-0 text-center font-mono text-[11px]",
          invalid && "border-destructive ring-1 ring-destructive",
        )}
        onChange={(event) => onChange(event.currentTarget.value)}
        onBlur={commit}
        onKeyDown={(event) => {
          if (event.key === "Enter") {
            event.preventDefault();
            commit();
          } else if (event.key === "Escape") {
            event.preventDefault();
            settledRef.current = true;
            onCancel();
          }
        }}
      />
    </div>
  );
}

function FolderTile({
  name,
  kind = "folder",
  editing = false,
  invalid = false,
  dragActive = false,
  onOpen,
  onAssetDropTarget,
  onClearAssetDropTarget,
  onMoveAssets,
  onRename,
  onDelete,
  confirmingDelete = false,
  deleteBody,
  onConfirmDelete,
  onCancelDelete,
  onCommitRename,
  onChangeRename,
  onCancelRename,
}: {
  name: string;
  kind?: "folder" | "root";
  editing?: boolean;
  invalid?: boolean;
  dragActive?: boolean;
  onOpen(): void;
  onAssetDropTarget(): void;
  onClearAssetDropTarget(): void;
  onMoveAssets(assetIds: string[]): void;
  onRename?(): void;
  onDelete?(): void;
  confirmingDelete?: boolean;
  deleteBody?: string;
  onConfirmDelete?(): void;
  onCancelDelete?(): void;
  onCommitRename?(name: string): void;
  onChangeRename?(): void;
  onCancelRename?(): void;
}) {
  const Icon = kind === "root" ? ArrowUp : Folder;
  const content = (
    <>
      <div className="flex aspect-square w-full items-center justify-center rounded-sm bg-muted">
        <Icon className="size-8 text-muted-foreground" />
      </div>
      {editing && onCommitRename && onCancelRename ? (
        <FolderNameInput
          initial={name}
          invalid={invalid}
          onChange={onChangeRename}
          onCommit={onCommitRename}
          onCancel={onCancelRename}
        />
      ) : (
        <span className="truncate px-0.5 text-center text-[11px] leading-tight text-foreground">
          {name}
        </span>
      )}
    </>
  );

  const tile = (
    <button
      type="button"
      data-asset-folder="true"
      className={cn(
        "flex w-[72px] flex-col gap-1 rounded-md border border-border bg-background p-1 text-left transition-colors hover:border-ring hover:bg-accent/40",
        dragActive && "border-ring bg-accent/60 ring-1 ring-ring",
      )}
      title={name}
      onDoubleClick={onOpen}
      onDragEnter={(event) => {
        if (event.dataTransfer.types.includes(ASSET_DND_MIME)) {
          onAssetDropTarget();
        }
      }}
      onDragOver={(event) => {
        if (event.dataTransfer.types.includes(ASSET_DND_MIME)) {
          event.preventDefault();
          event.dataTransfer.dropEffect = "move";
          onAssetDropTarget();
        }
      }}
      onDrop={(event) => {
        const payload = readAssetPayload(event.dataTransfer);
        if (!payload) {
          return;
        }
        event.preventDefault();
        event.stopPropagation();
        onClearAssetDropTarget();
        onMoveAssets(assetIdsFromPayload(payload));
      }}
      onDragEnd={onClearAssetDropTarget}
    >
      {content}
    </button>
  );

  if ((!onRename && !onDelete) || editing) {
    return (
      <div className="relative w-[72px]">
        {tile}
        {confirmingDelete && onConfirmDelete && onCancelDelete ? (
          <DeleteConfirm
            title={`Delete ${name}?`}
            body={deleteBody ?? "Moves contained assets to Root."}
            onConfirm={onConfirmDelete}
            onCancel={onCancelDelete}
          />
        ) : null}
      </div>
    );
  }

  return (
    <div className="relative w-[72px]">
      <ContextMenu>
        <ContextMenuTrigger asChild>{tile}</ContextMenuTrigger>
        <ContextMenuContent className="min-w-32">
          {onRename ? (
            <ContextMenuItem onSelect={onRename}>
              <Pen />
              Rename
            </ContextMenuItem>
          ) : null}
          {onDelete ? (
            <>
              {onRename ? <ContextMenuSeparator /> : null}
              <ContextMenuItem
                variant="destructive"
                className="bg-destructive/10 text-destructive focus:bg-destructive focus:text-destructive-foreground"
                onSelect={onDelete}
              >
                <Trash />
                Delete
              </ContextMenuItem>
            </>
          ) : null}
        </ContextMenuContent>
      </ContextMenu>
      {confirmingDelete && onConfirmDelete && onCancelDelete ? (
        <DeleteConfirm
          title={`Delete ${name}?`}
          body={deleteBody ?? "Moves contained assets to Root."}
          onConfirm={onConfirmDelete}
          onCancel={onCancelDelete}
        />
      ) : null}
    </div>
  );
}

interface PendingAssetDelete {
  asset: AssetEntry;
  body: string;
}

interface MarqueeState {
  startX: number;
  startY: number;
  currentX: number;
  currentY: number;
}

interface RectLike {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

function marqueeRect(marquee: MarqueeState): RectLike {
  return {
    left: Math.min(marquee.startX, marquee.currentX),
    top: Math.min(marquee.startY, marquee.currentY),
    right: Math.max(marquee.startX, marquee.currentX),
    bottom: Math.max(marquee.startY, marquee.currentY),
  };
}

function rectsIntersect(a: RectLike, b: RectLike): boolean {
  return a.left <= b.right && a.right >= b.left && a.top <= b.bottom && a.bottom >= b.top;
}

function cssEscape(value: string): string {
  return CSS.escape(value);
}

function MarqueeBox({ panel, marquee }: { panel: HTMLDivElement | null; marquee: MarqueeState }) {
  if (!panel) {
    return null;
  }
  const panelRect = panel.getBoundingClientRect();
  const rect = marqueeRect(marquee);
  return (
    <div
      className="pointer-events-none absolute border border-ring bg-ring/15"
      style={{
        left: `${rect.left - panelRect.left}px`,
        top: `${rect.top - panelRect.top}px`,
        width: `${rect.right - rect.left}px`,
        height: `${rect.bottom - rect.top}px`,
      }}
    />
  );
}

function FolderNameInput({
  initial,
  invalid = false,
  onChange,
  onCommit,
  onCancel,
}: {
  initial: string;
  invalid?: boolean;
  onChange?(): void;
  onCommit(name: string): void;
  onCancel(): void;
}) {
  const [value, setValue] = useState(initial);
  const inputRef = useRef<HTMLInputElement | null>(null);
  const settledRef = useRef(false);

  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      inputRef.current?.focus();
      inputRef.current?.select();
    });
    return () => cancelAnimationFrame(frame);
  }, []);

  const commit = (): void => {
    if (settledRef.current) {
      return;
    }
    settledRef.current = true;
    onCommit(value);
    window.setTimeout(() => {
      settledRef.current = false;
    }, 100);
  };

  return (
    <Input
      ref={inputRef}
      value={value}
      aria-invalid={invalid}
      className={cn(
        "h-5 rounded-sm px-1 py-0 text-center font-mono text-[11px]",
        invalid && "border-destructive ring-1 ring-destructive",
      )}
      onClick={(event) => event.stopPropagation()}
      onDoubleClick={(event) => event.stopPropagation()}
      onChange={(event) => {
        setValue(event.currentTarget.value);
        onChange?.();
      }}
      onBlur={commit}
      onKeyDown={(event) => {
        if (event.key === "Enter") {
          event.preventDefault();
          commit();
        } else if (event.key === "Escape") {
          event.preventDefault();
          settledRef.current = true;
          onCancel();
        }
      }}
    />
  );
}

type FolderItem =
  | { kind: "folder"; path: string; label: string; sortName: string }
  | { kind: "new"; path: string; label: string; sortName: string };

function sortedFolderItems(
  folders: string[],
  currentFolder: string | null,
  creatingFolder: boolean,
): FolderItem[] {
  const parentPrefix = currentFolder ? `${currentFolder}/` : "";
  const items: FolderItem[] = folders.flatMap((folder) => {
    if (currentFolder) {
      if (!folder.startsWith(parentPrefix)) {
        return [];
      }
      const rest = folder.slice(parentPrefix.length);
      if (!rest || rest.includes("/")) {
        return [];
      }
      return [{ kind: "folder", path: folder, label: rest, sortName: rest }];
    }
    if (folder.includes("/")) {
      return [];
    }
    return [{ kind: "folder", path: folder, label: folder, sortName: folder }];
  });
  if (creatingFolder) {
    items.push({ kind: "new", path: "\uffff", label: "", sortName: "\uffff" });
  }
  return items.sort((a, b) =>
    a.sortName.localeCompare(b.sortName, undefined, { numeric: true, sensitivity: "base" }),
  );
}

async function moveImportedAsset(assetId: string, folder: string): Promise<void> {
  if (assetId !== "0") {
    await client.moveAsset(assetId, folder);
  }
}

function assetIdsFromPayload(payload: ReturnType<typeof readAssetPayload>): string[] {
  if (!payload) {
    return [];
  }
  if (payload.ids && payload.ids.length > 0) {
    return payload.ids;
  }
  return payload.id ? [payload.id] : [];
}

/// Hit-test a physical-pixel drop position against the Assets panel's DOM rect.
/// (The drop event reports physical pixels; getBoundingClientRect is CSS pixels, so
/// we scale by devicePixelRatio.)
function isInsidePanel(position: { x: number; y: number }): boolean {
  const el = document.querySelector('[data-asset-panel="true"]');
  if (!el) {
    return false;
  }
  const rect = el.getBoundingClientRect();
  const scale = window.devicePixelRatio || 1;
  const cssX = position.x / scale;
  const cssY = position.y / scale;
  return cssX >= rect.left && cssX <= rect.right && cssY >= rect.top && cssY <= rect.bottom;
}
