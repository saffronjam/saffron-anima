/// The Assets panel: the React port of the C++ `assetCatalogPanel`
/// (editor_panels.cpp:226-325). A folder tree sidebar plus a responsive tile grid
/// over `store.assets`, navigated through a back/forward history and clickable
/// breadcrumbs, with an Import button (Tauri file dialog), an OS file-drop target,
/// and the View modal. Imports route by extension (parity with `importToCatalog`,
/// editor_app.cppm:188-205): images → import-texture (no spawn), everything else →
/// import-model (spawns + selects an entity, matching `se import-model`).
import { Fragment, useCallback, useEffect, useMemo, useRef, useState } from "react";
import type {
  DragEvent as ReactDragEvent,
  MouseEvent as ReactMouseEvent,
  PointerEvent as ReactPointerEvent,
} from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import { ArrowLeft, ArrowRight, Folder, FolderPlus, Pen, Plus, Trash } from "lucide-react";
import { client } from "../control/client";
import { invalidateThumbnails, useEditorStore, withNativeDialog } from "../state/store";
import { AssetTile } from "../components/AssetTile";
import {
  ASSET_DND_MIME,
  FOLDER_DND_MIME,
  assetIdsFromPayload,
  isCatalogDrag,
  readAssetPayload,
  readFolderPayload,
} from "../components/AssetTile";
import { AssetFolderTree, folderAncestorPaths, folderLabel } from "./AssetFolderTree";
import { matchesBinding } from "../lib/keybindings";
import { AssetMetadataPanel } from "../components/AssetMetadataPanel";
import { errorText, notify } from "../lib/flash";
import { useOutsideCommit } from "../lib/useOutsideCommit";
import type { AssetEntry, AssetMetadataDto, AssetUsageDto } from "../protocol";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { Input } from "@/components/ui/input";
import { ResizableHandle, ResizablePanel, ResizablePanelGroup } from "@/components/ui/resizable";
import { ScrollArea } from "@/components/ui/scroll-area";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";
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

/// Folder navigation history with browser semantics: navigating truncates the
/// forward tail; back/forward skip entries whose folder no longer exists.
interface FolderHistory {
  stack: (string | null)[];
  index: number;
}

/// A folder rename initiated from the grid or the tree; the origin picks which
/// surface renders the inline input so the two never both mount.
interface FolderActionTarget {
  path: string;
  origin: "grid" | "tree";
}

/// An in-progress folder creation: grid-initiated shows the inline tile in the
/// current folder, tree-initiated shows an inline row under `parent` in the tree.
interface CreatingFolder {
  parent: string | null;
  origin: "grid" | "tree";
}

/// The grid selection anchor for shift-range selection, across folders and assets.
type SelectionAnchor = { kind: "asset"; key: string } | { kind: "folder"; key: string } | null;

/// The nearest history entry in `step` direction that still exists; -1 if none.
function nearestHistoryIndex(history: FolderHistory, folders: string[], step: -1 | 1): number {
  for (let i = history.index + step; i >= 0 && i < history.stack.length; i += step) {
    const entry = history.stack[i];
    if (entry === null || folders.includes(entry)) {
      return i;
    }
  }
  return -1;
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
  const [history, setHistory] = useState<FolderHistory>({ stack: [null], index: 0 });
  const [creatingFolder, setCreatingFolder] = useState<CreatingFolder | null>(null);
  const [creatingFolderName, setCreatingFolderName] = useState("");
  const [renamingFolder, setRenamingFolder] = useState<FolderActionTarget | null>(null);
  const [pendingAssetDelete, setPendingAssetDelete] = useState<PendingAssetDelete | null>(null);
  const [pendingFolderDelete, setPendingFolderDelete] = useState<string | null>(null);
  const [selectedAssetIds, setSelectedAssetIds] = useState<Set<string>>(() => new Set());
  const [selectedFolderPaths, setSelectedFolderPaths] = useState<Set<string>>(() => new Set());
  const [selectionAnchor, setSelectionAnchor] = useState<SelectionAnchor>(null);
  const [folderError, setFolderError] = useState<string | null>(null);
  const [dropActive, setDropActive] = useState(false);
  const [assetDropTarget, setAssetDropTarget] = useState<string | null>(null);
  const [metadata, setMetadata] = useState<AssetMetadataDto | null>(null);
  const currentFolder = history.stack[history.index] ?? null;
  const visibleAssets = assets.filter((asset) => (asset.folder ?? "") === (currentFolder ?? ""));
  const detailAssetId =
    selectedAssetIds.size === 1 && selectedFolderPaths.size === 0 ? [...selectedAssetIds][0] : null;

  // The grid's selection order: folder tiles (sorted) then asset tiles, matching
  // the body's render order, so a shift-range can span folders and assets.
  const gridOrder = useMemo<{ kind: "asset" | "folder"; key: string }[]>(() => {
    const folderKeys = sortedFolderItems(folders, currentFolder, false).flatMap((item) =>
      item.kind === "folder" ? [{ kind: "folder" as const, key: item.path }] : [],
    );
    return [
      ...folderKeys,
      ...visibleAssets.map((asset) => ({ kind: "asset" as const, key: asset.id })),
    ];
  }, [folders, currentFolder, visibleAssets]);

  // Probe on-disk metadata for the single selected asset; ignore stale responses
  // when the selection changes mid-flight.
  useEffect(() => {
    if (!detailAssetId) {
      setMetadata(null);
      return;
    }
    let active = true;
    setMetadata(null);
    void client
      .probeAsset(detailAssetId)
      .then((meta) => {
        if (active) {
          setMetadata(meta);
        }
      })
      .catch(() => {});
    return () => {
      active = false;
    };
  }, [detailAssetId]);

  const navigateTo = useCallback((folder: string | null): void => {
    setHistory((current) => {
      if (current.stack[current.index] === folder) {
        return current;
      }
      const stack = [...current.stack.slice(0, current.index + 1), folder];
      return { stack, index: stack.length - 1 };
    });
  }, []);

  const goBack = useCallback((): void => {
    setHistory((current) => {
      const index = nearestHistoryIndex(current, folders, -1);
      return index < 0 ? current : { ...current, index };
    });
  }, [folders]);

  const goForward = useCallback((): void => {
    setHistory((current) => {
      const index = nearestHistoryIndex(current, folders, 1);
      return index < 0 ? current : { ...current, index };
    });
  }, [folders]);

  const canGoBack = nearestHistoryIndex(history, folders, -1) >= 0;
  const canGoForward = nearestHistoryIndex(history, folders, 1) >= 0;

  useEffect(() => {
    if (currentFolder && !folders.includes(currentFolder)) {
      navigateTo(null);
    }
  }, [currentFolder, folders, navigateTo]);

  useEffect(() => {
    const visibleIds = new Set(visibleAssets.map((asset) => asset.id));
    setSelectedAssetIds((current) => {
      const next = new Set([...current].filter((id) => visibleIds.has(id)));
      return next.size === current.size ? current : next;
    });
    setSelectionAnchor((current) =>
      current?.kind === "asset" && !visibleIds.has(current.key) ? null : current,
    );
  }, [visibleAssets]);

  useEffect(() => {
    const folderSet = new Set(folders);
    setSelectedFolderPaths((current) => {
      const next = new Set([...current].filter((path) => folderSet.has(path)));
      return next.size === current.size ? current : next;
    });
    setSelectionAnchor((current) =>
      current?.kind === "folder" && !folderSet.has(current.key) ? null : current,
    );
  }, [folders]);

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

  // Grid-initiated creation navigates to the parent so its inline tile is in view;
  // tree-initiated creation stays put and edits inline under the clicked row.
  const startCreateFolder = useCallback(
    (parent: string | null, origin: "grid" | "tree"): void => {
      if (origin === "grid") {
        navigateTo(parent);
      }
      setFolderError(null);
      setCreatingFolderName("");
      setCreatingFolder({ parent, origin });
    },
    [navigateTo],
  );

  const onNewFolder = useCallback((): void => {
    startCreateFolder(currentFolder, "grid");
  }, [currentFolder, startCreateFolder]);

  const startRenameFolder = useCallback((folder: string, origin: "grid" | "tree"): void => {
    setFolderError(null);
    setRenamingFolder({ path: folder, origin });
  }, []);

  const commitNewFolder = useCallback(
    async (name: string): Promise<void> => {
      const folder = normalizeFolderInput(name, creatingFolder?.parent ?? null);
      if (!folder) {
        setCreatingFolder(null);
        setCreatingFolderName("");
        setFolderError(null);
        return;
      }
      if (folders.includes(folder)) {
        setFolderError("Folder already exists");
        return;
      }
      try {
        for (const path of folderAncestorPaths(folder)) {
          if (!folders.includes(path)) {
            await client.createAssetFolder(path);
          }
        }
        await refreshAssets();
        setCreatingFolder(null);
        setCreatingFolderName("");
        setFolderError(null);
      } catch {
        setFolderError("Could not create folder");
      }
    },
    [creatingFolder, folders, refreshAssets],
  );

  const cancelNewFolder = useCallback((): void => {
    setCreatingFolder(null);
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
        setHistory((current) => ({
          ...current,
          stack: current.stack.map((entry) =>
            entry === null ? null : replaceFolderPrefix(entry, folder, next),
          ),
        }));
        setRenamingFolder(null);
        setFolderError(null);
      } catch {
        setFolderError("Could not rename folder");
      }
    },
    [folders, refreshAssets],
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

  // Re-parent dragged folders (keeping each label) under `parent`; a folder cannot
  // be dropped into itself or its own subtree, and a clashing name is skipped.
  const moveFoldersTo = useCallback(
    async (paths: string[], parent: string | null): Promise<void> => {
      const taken = new Set(folders);
      const moves: { from: string; to: string }[] = [];
      for (const folder of paths) {
        const to = parent ? `${parent}/${folderLabel(folder)}` : folderLabel(folder);
        if (to === folder || parent === folder || (parent && isFolderDescendant(parent, folder))) {
          continue;
        }
        if (taken.has(to)) {
          notify(`A folder named ${folderLabel(folder)} already exists there`);
          continue;
        }
        moves.push({ from: folder, to });
        taken.add(to);
      }
      if (moves.length === 0) {
        return;
      }
      for (const move of moves) {
        try {
          await client.renameAssetFolder(move.from, move.to);
        } catch (err) {
          notify(`Could not move ${folderLabel(move.from)}: ${errorText(err)}`);
        }
      }
      await refreshAssets();
      const rewrite = (path: string): string =>
        moves.reduce((acc, move) => replaceFolderPrefix(acc, move.from, move.to), path);
      setHistory((current) => ({
        ...current,
        stack: current.stack.map((entry) => (entry === null ? null : rewrite(entry))),
      }));
      setSelectedFolderPaths((current) => new Set([...current].map(rewrite)));
    },
    [folders, refreshAssets],
  );

  // The one selection entry point for grid tiles, folders and assets alike:
  // plain replaces, ctrl/meta toggles, shift extends the range from the anchor.
  const selectItem = useCallback(
    (kind: "asset" | "folder", key: string, event: ReactMouseEvent): void => {
      const index = gridOrder.findIndex((item) => item.kind === kind && item.key === key);
      if (index < 0) {
        return;
      }
      if (event.shiftKey && selectionAnchor) {
        const anchorIndex = gridOrder.findIndex(
          (item) => item.kind === selectionAnchor.kind && item.key === selectionAnchor.key,
        );
        if (anchorIndex >= 0) {
          const range = gridOrder.slice(
            Math.min(anchorIndex, index),
            Math.max(anchorIndex, index) + 1,
          );
          setSelectedAssetIds((current) => {
            const next = new Set(current);
            for (const item of range) {
              if (item.kind === "asset") {
                next.add(item.key);
              }
            }
            return next;
          });
          setSelectedFolderPaths((current) => {
            const next = new Set(current);
            for (const item of range) {
              if (item.kind === "folder") {
                next.add(item.key);
              }
            }
            return next;
          });
          setSelectionAnchor({ kind, key });
          return;
        }
      }
      const setSel = kind === "asset" ? setSelectedAssetIds : setSelectedFolderPaths;
      if (event.ctrlKey || event.metaKey) {
        setSel((current) => {
          const next = new Set(current);
          if (next.has(key)) {
            next.delete(key);
          } else {
            next.add(key);
          }
          return next;
        });
        setSelectionAnchor({ kind, key });
        return;
      }
      setSelectedAssetIds(kind === "asset" ? new Set([key]) : new Set());
      setSelectedFolderPaths(kind === "folder" ? new Set([key]) : new Set());
      setSelectionAnchor({ kind, key });
    },
    [gridOrder, selectionAnchor],
  );

  const selectAsset = useCallback(
    (asset: AssetEntry, event: ReactMouseEvent): void => selectItem("asset", asset.id, event),
    [selectItem],
  );

  const selectFolder = useCallback(
    (folder: string, event: ReactMouseEvent): void => selectItem("folder", folder, event),
    [selectItem],
  );

  // Assemble the drag payload from the live selection. If the dragged tile is not
  // part of it, the drag (and selection) collapses to just that tile.
  const beginDrag = useCallback(
    (kind: "asset" | "folder", key: string, event: ReactDragEvent): void => {
      const sel = kind === "asset" ? selectedAssetIds : selectedFolderPaths;
      let assetIds = [...selectedAssetIds];
      let folderPaths = [...selectedFolderPaths];
      if (!sel.has(key)) {
        assetIds = kind === "asset" ? [key] : [];
        folderPaths = kind === "folder" ? [key] : [];
        setSelectedAssetIds(new Set(assetIds));
        setSelectedFolderPaths(new Set(folderPaths));
        setSelectionAnchor({ kind, key });
      }
      const visibleIds = new Set(visibleAssets.map((asset) => asset.id));
      const folderSet = new Set(folders);
      assetIds = assetIds.filter((id) => visibleIds.has(id));
      folderPaths = folderPaths.filter((path) => folderSet.has(path));
      if (assetIds.length > 0) {
        event.dataTransfer.setData(ASSET_DND_MIME, JSON.stringify({ ids: assetIds }));
      }
      if (folderPaths.length > 0) {
        event.dataTransfer.setData(FOLDER_DND_MIME, JSON.stringify({ paths: folderPaths }));
      }
      event.dataTransfer.effectAllowed = "move";
    },
    [selectedAssetIds, selectedFolderPaths, visibleAssets, folders],
  );

  const requestDeleteAsset = useCallback(async (asset: AssetEntry): Promise<void> => {
    setPendingFolderDelete(null);
    let usages: AssetUsageDto[];
    try {
      usages = (await client.assetUsages(asset.id)).usages;
    } catch (err) {
      notify(`Could not look up usages: ${errorText(err)}`);
      return;
    }
    setPendingAssetDelete({ asset, usages });
  }, []);

  const confirmDeleteAsset = useCallback(
    async (asset: AssetEntry): Promise<void> => {
      setPendingAssetDelete(null);
      try {
        await client.deleteAsset(asset.id);
      } catch (err) {
        notify(`Could not delete ${asset.name}: ${errorText(err)}`);
        return;
      }
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
      setPendingFolderDelete(null);
      try {
        await client.deleteAssetFolder(folder);
      } catch (err) {
        notify(`Could not delete ${folderLabel(folder)}: ${errorText(err)}`);
        return;
      }
      if (
        currentFolder === folder ||
        (currentFolder && isFolderDescendant(currentFolder, folder))
      ) {
        navigateTo(null);
      }
      await refreshAssets();
    },
    [currentFolder, navigateTo, refreshAssets],
  );

  const cancelDelete = useCallback((): void => {
    setPendingAssetDelete(null);
    setPendingFolderDelete(null);
  }, []);

  // The single delete-confirmation modal, fed by whichever request is pending.
  const pendingDelete = pendingAssetDelete
    ? {
        title: `Delete ${pendingAssetDelete.asset.name}?`,
        body:
          pendingAssetDelete.usages.length > 0
            ? `Clears ${pendingAssetDelete.usages.length} usage${pendingAssetDelete.usages.length === 1 ? "" : "s"}:`
            : "Removes the catalog entry and imported file.",
        usages: pendingAssetDelete.usages,
        confirm: () => void confirmDeleteAsset(pendingAssetDelete.asset),
      }
    : pendingFolderDelete !== null
      ? {
          title: `Delete ${folderLabel(pendingFolderDelete)}?`,
          body: deleteFolderBody(pendingFolderDelete),
          usages: [],
          confirm: () => void confirmDeleteFolder(pendingFolderDelete),
        }
      : null;
  const shownUsages = pendingDelete?.usages.slice(0, MAX_USAGE_LINES) ?? [];
  const extraUsages = (pendingDelete?.usages.length ?? 0) - shownUsages.length;

  return (
    <div className="flex h-full min-h-0 flex-col" data-asset-panel="true">
      <div className="flex h-10 flex-none items-center gap-1 border-b border-border px-3">
        <span className="mr-1 flex-none text-xs font-semibold uppercase tracking-wide text-muted-foreground">
          Assets
        </span>
        <Button
          type="button"
          size="icon-xs"
          variant="ghost"
          className="flex-none"
          disabled={!canGoBack}
          onClick={goBack}
          aria-label="Back"
        >
          <ArrowLeft />
        </Button>
        <Button
          type="button"
          size="icon-xs"
          variant="ghost"
          className="flex-none"
          disabled={!canGoForward}
          onClick={goForward}
          aria-label="Forward"
        >
          <ArrowRight />
        </Button>
        <Breadcrumbs
          currentFolder={currentFolder}
          onNavigate={navigateTo}
          onMoveAssets={(assetIds, folder) => void moveAssetsToFolder(assetIds, folder)}
          onMoveFolders={(paths, parent) => void moveFoldersTo(paths, parent)}
        />
        <div className="ml-auto flex flex-none items-center gap-2">
          <Button type="button" size="sm" variant="outline" className="gap-1" onClick={onNewFolder}>
            <FolderPlus />
            New Folder
          </Button>
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="sm"
                variant="outline"
                className="gap-1"
                onClick={() => void onImportClick()}
                disabled={nativeDialogOpen}
              >
                <Plus />
                Import
              </Button>
            </TooltipTrigger>
            <TooltipContent>Import a model or texture</TooltipContent>
          </Tooltip>
        </div>
      </div>
      <ResizablePanelGroup orientation="horizontal" className="min-h-0 flex-1">
        <ResizablePanel
          defaultSize={220}
          minSize={140}
          maxSize={480}
          groupResizeBehavior="preserve-pixel-size"
          className="min-w-0"
        >
          <ContextMenu modal={false}>
            <ContextMenuTrigger asChild>
              <div className="h-full min-h-0">
                <AssetFolderTree
                  folders={folders}
                  currentFolder={currentFolder}
                  renamingFolder={renamingFolder?.origin === "tree" ? renamingFolder.path : null}
                  renameInvalid={folderError !== null}
                  creatingIn={
                    creatingFolder?.origin === "tree" ? { parent: creatingFolder.parent } : null
                  }
                  createInvalid={folderError !== null}
                  onNavigate={navigateTo}
                  onMoveAssets={(assetIds, folder) => void moveAssetsToFolder(assetIds, folder)}
                  onMoveFolders={(paths, parent) => void moveFoldersTo(paths, parent)}
                  onNewFolder={(parent) => startCreateFolder(parent, "tree")}
                  onStartRename={(folder) => startRenameFolder(folder, "tree")}
                  onChangeRename={() => setFolderError(null)}
                  onCommitRename={(folder, name) => void commitRenameFolder(folder, name)}
                  onCancelRename={cancelRenameFolder}
                  onChangeCreate={() => setFolderError(null)}
                  onCommitCreate={(name) => void commitNewFolder(name)}
                  onCancelCreate={cancelNewFolder}
                  onDelete={requestDeleteFolder}
                />
              </div>
            </ContextMenuTrigger>
            {/* No focus restore on close: it lands after the inline name input takes
                focus (the menu unmounts post-exit-animation) and would blur-cancel it. */}
            <ContextMenuContent
              className="min-w-40"
              onCloseAutoFocus={(event) => event.preventDefault()}
            >
              <ContextMenuItem onSelect={() => startCreateFolder(null, "tree")}>
                <Folder />
                New Folder
              </ContextMenuItem>
              <ContextMenuItem onSelect={() => void onImportClick()} disabled={nativeDialogOpen}>
                <Plus />
                Import
              </ContextMenuItem>
            </ContextMenuContent>
          </ContextMenu>
        </ResizablePanel>
        <ResizableHandle />
        <ResizablePanel className="relative min-w-0 overflow-hidden">
          <ContextMenu modal={false}>
            <ContextMenuTrigger asChild>
              <div className="h-full min-h-0">
                <AssetPanelBody
                  assets={visibleAssets}
                  folders={folders}
                  currentFolder={currentFolder}
                  dropActive={dropActive}
                  creatingFolder={creatingFolder?.origin === "grid"}
                  creatingFolderName={creatingFolderName}
                  renamingFolder={renamingFolder}
                  folderError={folderError}
                  selectedAssetIds={selectedAssetIds}
                  selectedFolderPaths={selectedFolderPaths}
                  assetDropTarget={assetDropTarget}
                  onOpenFolder={navigateTo}
                  onView={openAssetTab}
                  onSelectAsset={selectAsset}
                  onSelectFolder={selectFolder}
                  onBeginDrag={beginDrag}
                  onDeleteAsset={(asset) => void requestDeleteAsset(asset)}
                  onDeleteFolder={requestDeleteFolder}
                  onMoveAssets={(assetIds, folder) => void moveAssetsToFolder(assetIds, folder)}
                  onMoveFolders={(paths, parent) => void moveFoldersTo(paths, parent)}
                  onSetMarqueeSelection={({ assetIds, folderPaths }) => {
                    setSelectedAssetIds(new Set(assetIds));
                    setSelectedFolderPaths(new Set(folderPaths));
                    const lastAsset = assetIds.at(-1);
                    const lastFolder = folderPaths.at(-1);
                    setSelectionAnchor(
                      lastAsset
                        ? { kind: "asset", key: lastAsset }
                        : lastFolder
                          ? { kind: "folder", key: lastFolder }
                          : null,
                    );
                  }}
                  onCommitNewFolder={(name) => void commitNewFolder(name)}
                  onChangeNewFolderName={setCreatingFolderName}
                  onCancelNewFolder={cancelNewFolder}
                  onStartRenameFolder={(folder) => startRenameFolder(folder, "grid")}
                  onCommitRenameFolder={(folder, name) => void commitRenameFolder(folder, name)}
                  onCancelRenameFolder={cancelRenameFolder}
                  onAssetDropTarget={setAssetDropTarget}
                  onClearFolderError={() => setFolderError(null)}
                />
              </div>
            </ContextMenuTrigger>
            <ContextMenuContent
              className="min-w-40"
              onCloseAutoFocus={(event) => event.preventDefault()}
            >
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
          <AssetMetadataPanel
            metadata={metadata}
            open={detailAssetId !== null}
            onClose={() => {
              setSelectedAssetIds(new Set());
              setSelectionAnchor(null);
            }}
          />
        </ResizablePanel>
      </ResizablePanelGroup>
      <Dialog
        open={pendingDelete !== null}
        onOpenChange={(dialogOpen) => {
          if (!dialogOpen) {
            cancelDelete();
          }
        }}
      >
        <DialogContent showCloseButton={false} className="sm:max-w-sm">
          <DialogHeader>
            <DialogTitle>{pendingDelete?.title}</DialogTitle>
            <DialogDescription>{pendingDelete?.body}</DialogDescription>
          </DialogHeader>
          {shownUsages.length > 0 ? (
            <div className="text-sm text-muted-foreground">
              <ul className="list-disc space-y-1 pl-5">
                {shownUsages.map((usage) => (
                  <li key={`${usage.entity ?? ""}-${usage.slot}`}>
                    <span className="flex items-center gap-1.5">
                      {usage.entityName ? (
                        <span className="truncate">{usage.entityName}</span>
                      ) : null}
                      <Badge variant="secondary">{usage.slot}</Badge>
                    </span>
                  </li>
                ))}
              </ul>
              {extraUsages > 0 ? (
                <p className="mt-2">
                  And {extraUsages} additional usage{extraUsages === 1 ? "" : "s"}
                </p>
              ) : null}
            </div>
          ) : null}
          <DialogFooter>
            <Button type="button" variant="outline" onClick={cancelDelete}>
              Cancel
            </Button>
            <Button type="button" variant="destructive" onClick={() => pendingDelete?.confirm()}>
              Delete
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </div>
  );
}

/// The clickable path: Root plus one segment per folder level, each navigating to
/// its prefix and accepting an asset drop (the move-to-root affordance the old Root
/// tile provided).
function Breadcrumbs({
  currentFolder,
  onNavigate,
  onMoveAssets,
  onMoveFolders,
}: {
  currentFolder: string | null;
  onNavigate(folder: string | null): void;
  onMoveAssets(assetIds: string[], folder: string | null): void;
  onMoveFolders(paths: string[], parent: string | null): void;
}) {
  const [dropTarget, setDropTarget] = useState<string | null>(null);
  const segments = currentFolder ? currentFolder.split("/") : [];
  const crumbs: { key: string; label: string; target: string | null }[] = [
    { key: "", label: "Root", target: null },
    ...segments.map((segment, i) => {
      const prefix = segments.slice(0, i + 1).join("/");
      return { key: prefix, label: segment, target: prefix };
    }),
  ];
  return (
    <div className="ml-1 flex min-w-0 items-center gap-0.5 overflow-hidden text-xs text-muted-foreground">
      {crumbs.map((crumb, i) => (
        <Fragment key={crumb.key}>
          {i > 0 ? <span className="flex-none">/</span> : null}
          <Button
            type="button"
            size="xs"
            variant="ghost"
            className={cn(
              "max-w-40 truncate px-1 font-mono",
              i === crumbs.length - 1 && i > 0 && "text-foreground",
              dropTarget === crumb.key && "bg-accent/60 ring-1 ring-ring",
            )}
            onClick={() => onNavigate(crumb.target)}
            onDragEnter={(event) => {
              if (isCatalogDrag(event.dataTransfer)) {
                setDropTarget(crumb.key);
              }
            }}
            onDragOver={(event) => {
              if (isCatalogDrag(event.dataTransfer)) {
                event.preventDefault();
                event.dataTransfer.dropEffect = "move";
                setDropTarget(crumb.key);
              }
            }}
            onDragLeave={() => {
              setDropTarget((current) => (current === crumb.key ? null : current));
            }}
            onDrop={(event) => {
              const ids = assetIdsFromPayload(readAssetPayload(event.dataTransfer));
              const folderPaths = readFolderPayload(event.dataTransfer);
              if (ids.length === 0 && folderPaths.length === 0) {
                return;
              }
              event.preventDefault();
              setDropTarget(null);
              if (folderPaths.length > 0) {
                onMoveFolders(folderPaths, crumb.target);
              }
              if (ids.length > 0) {
                onMoveAssets(ids, crumb.target);
              }
            }}
          >
            {crumb.label}
          </Button>
        </Fragment>
      ))}
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
  selectedAssetIds,
  selectedFolderPaths,
  assetDropTarget,
  onOpenFolder,
  onView,
  onSelectAsset,
  onSelectFolder,
  onBeginDrag,
  onDeleteAsset,
  onDeleteFolder,
  onMoveAssets,
  onMoveFolders,
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
  renamingFolder: FolderActionTarget | null;
  folderError: string | null;
  selectedAssetIds: Set<string>;
  selectedFolderPaths: Set<string>;
  assetDropTarget: string | null;
  onOpenFolder(folder: string | null): void;
  onView(asset: AssetEntry): void;
  onSelectAsset(asset: AssetEntry, event: ReactMouseEvent): void;
  onSelectFolder(folder: string, event: ReactMouseEvent): void;
  onBeginDrag(kind: "asset" | "folder", key: string, event: ReactDragEvent): void;
  onDeleteAsset(asset: AssetEntry): void;
  onDeleteFolder(folder: string): void;
  onMoveAssets(assetIds: string[], folder: string | null): void;
  onMoveFolders(paths: string[], parent: string | null): void;
  onSetMarqueeSelection(sel: { assetIds: string[]; folderPaths: string[] }): void;
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
  const blank = !creatingFolder && folderItems.length === 0 && assets.length === 0;
  const empty = blank && !currentFolder;
  const folderEmpty = blank && currentFolder !== null;
  const panelRef = useRef<HTMLDivElement | null>(null);
  const [marquee, setMarquee] = useState<MarqueeState | null>(null);

  const selectMarqueeAssets = (next: MarqueeState): void => {
    const panel = panelRef.current;
    if (!panel) {
      return;
    }
    const rect = marqueeRect(next);
    const hits = (el: Element | null): boolean =>
      el instanceof HTMLElement && rectsIntersect(rect, el.getBoundingClientRect());
    const assetIds = assets
      .filter((asset) => hits(panel.querySelector(`[data-asset-tile-id="${cssEscape(asset.id)}"]`)))
      .map((asset) => asset.id);
    const folderPaths = folderItems.flatMap((item) =>
      item.kind === "folder" &&
      hits(panel.querySelector(`[data-asset-folder-path="${cssEscape(item.path)}"]`))
        ? [item.path]
        : [],
    );
    onSetMarqueeSelection({ assetIds, folderPaths });
  };

  const startMarquee = (event: ReactPointerEvent<HTMLDivElement>): void => {
    if (event.button !== 0) {
      return;
    }
    const target = event.target;
    // Portaled overlays (context menus) bubble here through the React tree but are
    // not DOM descendants; capturing their pointer would swallow the item's click.
    if (!(target instanceof Element) || !event.currentTarget.contains(target)) {
      return;
    }
    if (target.closest("[data-asset-item='true'], [data-asset-folder='true'], button, input")) {
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
    onSetMarqueeSelection({ assetIds: [], folderPaths: [] });
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
      className={cn(
        "relative h-full min-h-0",
        dropActive && "rounded-sm ring-2 ring-inset ring-ring",
      )}
      onPointerDown={startMarquee}
      onPointerMove={moveMarquee}
      onPointerUp={endMarquee}
      onPointerCancel={endMarquee}
      onDragOver={(event) => {
        if (isCatalogDrag(event.dataTransfer)) {
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
        const ids = assetIdsFromPayload(readAssetPayload(event.dataTransfer));
        const folderPaths = readFolderPayload(event.dataTransfer);
        if (ids.length === 0 && folderPaths.length === 0) {
          return;
        }
        event.preventDefault();
        onAssetDropTarget(null);
        if (folderPaths.length > 0) {
          onMoveFolders(folderPaths, currentFolder);
        }
        if (ids.length > 0) {
          onMoveAssets(ids, currentFolder);
        }
      }}
    >
      <ScrollArea className="h-full">
        <div className="min-h-full p-2">
          {empty ? (
            <p className="px-1 py-3 text-center text-xs italic text-muted-foreground">
              No assets yet. Import or drag-and-drop a model or texture.
            </p>
          ) : folderEmpty ? (
            <p className="px-1 py-3 text-center text-xs italic text-muted-foreground">
              This folder is empty. Drag assets here to move them.
            </p>
          ) : (
            <div
              className="grid gap-2"
              style={{ gridTemplateColumns: "repeat(auto-fill, minmax(72px, 1fr))" }}
            >
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
                    path={item.path}
                    name={item.label}
                    editing={renamingFolder?.origin === "grid" && renamingFolder.path === item.path}
                    invalid={folderError !== null && renamingFolder?.path === item.path}
                    selected={selectedFolderPaths.has(item.path)}
                    dragActive={assetDropTarget === item.path}
                    onOpen={() => onOpenFolder(item.path)}
                    onSelect={(event) => onSelectFolder(item.path, event)}
                    onBeginDrag={(event) => onBeginDrag("folder", item.path, event)}
                    onAssetDropTarget={() => onAssetDropTarget(item.path)}
                    onClearAssetDropTarget={() => onAssetDropTarget(null)}
                    onMoveAssets={(assetIds) => onMoveAssets(assetIds, item.path)}
                    onMoveFolders={(paths) => onMoveFolders(paths, item.path)}
                    onRename={() => onStartRenameFolder(item.path)}
                    onDelete={() => onDeleteFolder(item.path)}
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
                  onBeginDrag={(entry, event) => onBeginDrag("asset", entry.id, event)}
                />
              ))}
            </div>
          )}
        </div>
      </ScrollArea>
      {marquee ? <MarqueeBox panel={panelRef.current} marquee={marquee} /> : null}
    </div>
  );
}

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

  useOutsideCommit(inputRef, commit);

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
  path,
  name,
  editing = false,
  invalid = false,
  selected = false,
  dragActive = false,
  onOpen,
  onSelect,
  onBeginDrag,
  onAssetDropTarget,
  onClearAssetDropTarget,
  onMoveAssets,
  onMoveFolders,
  onRename,
  onDelete,
  onCommitRename,
  onChangeRename,
  onCancelRename,
}: {
  path: string;
  name: string;
  editing?: boolean;
  invalid?: boolean;
  selected?: boolean;
  dragActive?: boolean;
  onOpen(): void;
  onSelect?(event: ReactMouseEvent): void;
  onBeginDrag(event: ReactDragEvent): void;
  onAssetDropTarget(): void;
  onClearAssetDropTarget(): void;
  onMoveAssets(assetIds: string[]): void;
  onMoveFolders?(paths: string[]): void;
  onRename?(): void;
  onDelete?(): void;
  onCommitRename?(name: string): void;
  onChangeRename?(): void;
  onCancelRename?(): void;
}) {
  const content = (
    <>
      <div className="flex aspect-square w-full items-center justify-center rounded-sm bg-muted">
        <Folder className="size-8 text-muted-foreground" />
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
      data-asset-folder-path={path}
      className={cn(
        "flex w-[72px] flex-col gap-1 rounded-md border border-border bg-background p-1 text-left transition-colors hover:border-ring hover:bg-accent/40",
        selected && "border-ring bg-accent/60 ring-1 ring-ring",
        dragActive && "border-ring bg-accent/60 ring-1 ring-ring",
      )}
      draggable={!editing}
      onClick={(event) => onSelect?.(event)}
      onDoubleClick={onOpen}
      // The configured delete key on the focused tile runs the same delete as the
      // context menu.
      onKeyDown={(event) => {
        if (
          !editing &&
          onDelete &&
          matchesBinding(event, "assets.delete", useEditorStore.getState().keyBindings)
        ) {
          event.preventDefault();
          onDelete();
        }
      }}
      onDragStart={onBeginDrag}
      onDragEnter={(event) => {
        if (isCatalogDrag(event.dataTransfer)) {
          onAssetDropTarget();
        }
      }}
      onDragOver={(event) => {
        if (isCatalogDrag(event.dataTransfer)) {
          event.preventDefault();
          event.dataTransfer.dropEffect = "move";
          onAssetDropTarget();
        }
      }}
      onDrop={(event) => {
        const ids = assetIdsFromPayload(readAssetPayload(event.dataTransfer));
        const folderPaths = readFolderPayload(event.dataTransfer);
        if (ids.length === 0 && folderPaths.length === 0) {
          return;
        }
        event.preventDefault();
        event.stopPropagation();
        onClearAssetDropTarget();
        if (folderPaths.length > 0) {
          onMoveFolders?.(folderPaths);
        }
        if (ids.length > 0) {
          onMoveAssets(ids);
        }
      }}
      onDragEnd={onClearAssetDropTarget}
    >
      {content}
    </button>
  );

  if ((!onRename && !onDelete) || editing) {
    return <div className="relative w-[72px]">{tile}</div>;
  }

  return (
    <div className="relative w-[72px]" onContextMenu={(event) => event.stopPropagation()}>
      <ContextMenu>
        <ContextMenuTrigger asChild>{tile}</ContextMenuTrigger>
        <ContextMenuContent
          className="min-w-32"
          onCloseAutoFocus={(event) => event.preventDefault()}
        >
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
    </div>
  );
}

/// Usage lines shown in the delete dialog before collapsing into "And X additional".
const MAX_USAGE_LINES = 5;

interface PendingAssetDelete {
  asset: AssetEntry;
  usages: AssetUsageDto[];
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

  useOutsideCommit(inputRef, commit);

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
