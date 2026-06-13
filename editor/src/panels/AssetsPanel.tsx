/// The Assets panel: the React port of the C++ `assetCatalogPanel`
/// (editor_panels.cpp:226-325). A folder tree sidebar plus a responsive tile grid
/// over `store.assets`, navigated through a back/forward history and clickable
/// breadcrumbs, with an Import button (Tauri file dialog), an OS file-drop target,
/// and the View modal. Imports route by extension (parity with `importToCatalog`,
/// editor_app.cppm:188-205): images → import-texture (no spawn), everything else →
/// import-model (spawns + selects an entity, matching `se import-model`).
import { Fragment, memo, useCallback, useEffect, useMemo, useRef, useState } from "react";
import type {
  DragEvent as ReactDragEvent,
  MouseEvent as ReactMouseEvent,
  PointerEvent as ReactPointerEvent,
} from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import {
  ArrowLeft,
  ArrowRight,
  Eye,
  Folder,
  FolderPlus,
  Pen,
  Pencil,
  Plus,
  Trash,
} from "lucide-react";
import { client } from "../control/client";
import { invalidateThumbnails, useEditorStore, withNativeDialog } from "../state/store";
import type { AssetGridItem } from "../state/store";
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
import { logRender } from "../lib/renderLog";
import { matchesBinding } from "../lib/keybindings";
import { AssetMetadataPanel } from "../components/AssetMetadataPanel";
import { errorText, notify, notifyError } from "../lib/flash";
import { useOutsideCommit } from "../lib/useOutsideCommit";
import { useElementSize } from "../lib/useElementSize";
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

function assetCountLabel(count: number): string {
  return `${count} asset${count === 1 ? "" : "s"}`;
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

/// The grid tile under the last contextmenu event, resolved from the tile DOM
/// attributes before Radix opens the shared menu; null = the empty area.
type GridMenuTarget = { kind: "asset"; id: string } | { kind: "folder"; path: string } | null;

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
        await moveImportedAsset(imported.id, folder);
      }
    }
  } catch {
    // The engine reports a bad import as ok:false (a rejected promise); swallow so
    // a single bad file doesn't break a multi-file drop.
  }
}

export function AssetsPanel() {
  logRender("AssetsPanel");
  const assets = useEditorStore((s) => s.assets);
  const folders = useEditorStore((s) => s.assetFolders);
  const refreshAssets = useEditorStore((s) => s.refreshAssets);
  const instantiateModel = useEditorStore((s) => s.instantiateModel);
  const nativeDialogOpen = useEditorStore((s) => s.nativeDialogOpen);
  const openImageViewerTab = useEditorStore((s) => s.openImageViewerTab);
  const openAssetEditorForAsset = useEditorStore((s) => s.openAssetEditorForAsset);
  const closeViewTab = useEditorStore((s) => s.closeViewTab);
  const [history, setHistory] = useState<FolderHistory>({ stack: [null], index: 0 });
  const [creatingFolder, setCreatingFolder] = useState<CreatingFolder | null>(null);
  const [creatingFolderName, setCreatingFolderName] = useState("");
  const [renamingFolder, setRenamingFolder] = useState<FolderActionTarget | null>(null);
  const [renamingAsset, setRenamingAsset] = useState<string | null>(null);
  const menuTargetRef = useRef<GridMenuTarget>(null);
  const [pendingAssetDelete, setPendingAssetDelete] = useState<PendingAssetDelete | null>(null);
  const [pendingFolderDelete, setPendingFolderDelete] = useState<string | null>(null);
  // Grid selection lives in the store so tiles subscribe to their own membership;
  // this panel deliberately never reads it at render time (event handlers use
  // getState()), so a selection delta re-renders tiles, not the panel.
  const selectAssetGridItem = useEditorStore((s) => s.selectAssetGridItem);
  const pruneAssetSelection = useEditorStore((s) => s.pruneAssetSelection);
  const removeFromAssetSelection = useEditorStore((s) => s.removeFromAssetSelection);
  const rewriteSelectedFolderPaths = useEditorStore((s) => s.rewriteSelectedFolderPaths);
  const [folderError, setFolderError] = useState<string | null>(null);
  const [dropActive, setDropActive] = useState(false);
  const [assetDropTarget, setAssetDropTarget] = useState<string | null>(null);
  const currentFolder = history.stack[history.index] ?? null;
  // Embedded sub-assets (a `.smodel`'s mesh/material/texture rows) are hidden from the top level so a
  // model imports as ONE tile, not a flood — they resolve through their container by (modelId, subId).
  const visibleAssets = useMemo(
    () =>
      assets.filter((asset) => (asset.folder ?? "") === (currentFolder ?? "") && !asset.container),
    [assets, currentFolder],
  );

  // The grid's selection order: folder tiles (sorted) then asset tiles, matching
  // the body's render order, so a shift-range can span folders and assets.
  const gridOrder = useMemo<AssetGridItem[]>(() => {
    const folderKeys = sortedFolderItems(folders, currentFolder, false).flatMap((item) =>
      item.kind === "folder" ? [{ kind: "folder" as const, key: item.path }] : [],
    );
    return [
      ...folderKeys,
      ...visibleAssets.map((asset) => ({ kind: "asset" as const, key: asset.id })),
    ];
  }, [folders, currentFolder, visibleAssets]);

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

  // Prune the selection (and a rename-in-progress) when assets leave the visible
  // grid or folders cease to exist; the store action bails out identity-stable, so
  // the StrictMode double-run is a no-op.
  useEffect(() => {
    pruneAssetSelection(visibleAssets, folders);
    setRenamingAsset((current) =>
      current !== null && !visibleAssets.some((asset) => asset.id === current) ? null : current,
    );
  }, [visibleAssets, folders, pruneAssetSelection]);

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

  const endRenameAsset = useCallback((): void => {
    setRenamingAsset(null);
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

  const commitNewFolderName = useCallback(
    (name: string): void => void commitNewFolder(name),
    [commitNewFolder],
  );

  const commitRenameFolderName = useCallback(
    (folder: string, name: string): void => void commitRenameFolder(folder, name),
    [commitRenameFolder],
  );

  const clearFolderError = useCallback((): void => setFolderError(null), []);

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
      rewriteSelectedFolderPaths(rewrite);
    },
    [folders, refreshAssets, rewriteSelectedFolderPaths],
  );

  // Stable void-returning prop shapes of the async handlers above, so the memo'd
  // body and tiles see one identity across renders.
  const moveAssets = useCallback(
    (assetIds: string[], folder: string | null): void => void moveAssetsToFolder(assetIds, folder),
    [moveAssetsToFolder],
  );

  const moveFolders = useCallback(
    (paths: string[], parent: string | null): void => void moveFoldersTo(paths, parent),
    [moveFoldersTo],
  );

  // The one selection entry point for grid tiles, folders and assets alike (the
  // store action holds the plain/toggle/shift-range semantics and the anchor).
  const selectGridItem = useCallback(
    (kind: "asset" | "folder", key: string, event: ReactMouseEvent): void =>
      selectAssetGridItem(
        kind,
        key,
        { shift: event.shiftKey, toggle: event.ctrlKey || event.metaKey },
        gridOrder,
      ),
    [selectAssetGridItem, gridOrder],
  );

  const selectAsset = useCallback(
    (asset: AssetEntry, event: ReactMouseEvent): void => selectGridItem("asset", asset.id, event),
    [selectGridItem],
  );

  const selectFolder = useCallback(
    (folder: string, event: ReactMouseEvent): void => selectGridItem("folder", folder, event),
    [selectGridItem],
  );

  // Assemble the drag payload from the live selection (read at event time, so the
  // callback identity never tracks it). If the dragged tile is not part of it, the
  // drag carries just that tile without changing selection; click owns selection.
  const beginDrag = useCallback(
    (kind: "asset" | "folder", key: string, event: ReactDragEvent): void => {
      const state = useEditorStore.getState();
      const sel = kind === "asset" ? state.selectedAssetIds : state.selectedFolderPaths;
      let assetIds = [...state.selectedAssetIds];
      let folderPaths = [...state.selectedFolderPaths];
      if (!sel.has(key)) {
        assetIds = kind === "asset" ? [key] : [];
        folderPaths = kind === "folder" ? [key] : [];
      }
      const visibleIds = new Set(
        state.assets
          .filter((asset) => (asset.folder ?? "") === (currentFolder ?? ""))
          .map((asset) => asset.id),
      );
      const folderSet = new Set(state.assetFolders);
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
    [currentFolder],
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
    setPendingAssetDelete({ assets: [asset], usages });
  }, []);

  const deleteAsset = useCallback(
    (asset: AssetEntry): void => void requestDeleteAsset(asset),
    [requestDeleteAsset],
  );

  /// Place a model asset into the scene (the scene poll picks up the new entity).
  const onInstantiate = useCallback(
    (modelId: string): void => {
      void instantiateModel(modelId)
        .then(() => notify("Added to scene"))
        .catch((err: unknown) => notifyError(errorText(err)));
    },
    [instantiateModel],
  );

  const requestDeleteAssets = useCallback(async (targetAssets: AssetEntry[]): Promise<void> => {
    if (targetAssets.length === 0) {
      return;
    }
    setPendingFolderDelete(null);
    const usages: AssetUsageDto[] = [];
    let failed = false;
    await targetAssets.reduce(
      (prev, asset) =>
        prev.then(async () => {
          if (failed) {
            return;
          }
          try {
            usages.push(...(await client.assetUsages(asset.id)).usages);
          } catch (err) {
            failed = true;
            notify(`Could not look up usages for ${asset.name}: ${errorText(err)}`);
          }
        }),
      Promise.resolve(),
    );
    if (failed) {
      return;
    }
    setPendingAssetDelete({ assets: targetAssets, usages });
  }, []);

  // Double-click / "View" routing: every model, mesh, and animation clip opens the asset editor (the
  // live preview viewport, plus rig panels when the model is rigged); textures and other assets open
  // the image viewer.
  const routeView = useCallback(
    (asset: AssetEntry) => {
      const ridesAssetEditor =
        asset.type === "model" || asset.type === "mesh" || asset.type === "animation";
      if (ridesAssetEditor) {
        openAssetEditorForAsset(asset.id, asset.name);
      } else {
        openImageViewerTab(asset);
      }
    },
    [openImageViewerTab, openAssetEditorForAsset],
  );

  const confirmDeleteAssets = useCallback(
    async (targetAssets: AssetEntry[]): Promise<void> => {
      setPendingAssetDelete(null);
      const deletedIds = new Set<string>();
      await targetAssets.reduce(
        (prev, asset) =>
          prev.then(async () => {
            try {
              await client.deleteAsset(asset.id);
              deletedIds.add(asset.id);
              closeViewTab(`imageViewer:${asset.id}`);
              closeViewTab(`assetEditor:${asset.id}`);
            } catch (err) {
              notify(`Could not delete ${asset.name}: ${errorText(err)}`);
            }
          }),
        Promise.resolve(),
      );
      if (deletedIds.size > 0) {
        invalidateThumbnails();
        removeFromAssetSelection(deletedIds);
        await refreshAssets();
      }
    },
    [closeViewTab, refreshAssets, removeFromAssetSelection],
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
    ? (() => {
        const count = pendingAssetDelete.assets.length;
        return {
          title:
            count === 1
              ? `Delete ${pendingAssetDelete.assets[0].name}?`
              : `Delete ${assetCountLabel(count)}?`,
          body:
            pendingAssetDelete.usages.length > 0
              ? `Clears ${pendingAssetDelete.usages.length} usage${pendingAssetDelete.usages.length === 1 ? "" : "s"}:`
              : count === 1
                ? "Removes the catalog entry and imported file."
                : `Removes ${assetCountLabel(count)} from the catalog and deletes their imported files.`,
          usages: pendingAssetDelete.usages,
          confirm: () => void confirmDeleteAssets(pendingAssetDelete.assets),
        };
      })()
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

  // The grid area's width drives the Details pane: docked narrow (e.g. on the right) it moves to
  // the bottom so a 256px side pane never swallows the grid. Seed wide (right) for the first paint.
  const gridRef = useRef<HTMLDivElement>(null);
  const gridSize = useElementSize(gridRef);
  const detailOrientation: "right" | "bottom" =
    gridSize.width > 0 && gridSize.width < 480 ? "bottom" : "right";

  return (
    <div className="flex h-full min-h-0 flex-col" data-asset-panel="true">
      <div className="flex h-10 flex-none items-center gap-1 border-b border-border px-3">
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
          onMoveAssets={moveAssets}
          onMoveFolders={moveFolders}
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
                  onMoveAssets={moveAssets}
                  onMoveFolders={moveFolders}
                  onNewFolder={(parent) => startCreateFolder(parent, "tree")}
                  onStartRename={(folder) => startRenameFolder(folder, "tree")}
                  onChangeRename={clearFolderError}
                  onCommitRename={commitRenameFolderName}
                  onCancelRename={cancelRenameFolder}
                  onChangeCreate={clearFolderError}
                  onCommitCreate={commitNewFolderName}
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
              <div
                ref={gridRef}
                className="h-full min-h-0"
                // Resolve the tile under a right-click into the ref before Radix
                // opens the one shared menu; the menu items read it at open time.
                onContextMenu={(event) => {
                  const target = event.target instanceof Element ? event.target : null;
                  const assetId =
                    target?.closest<HTMLElement>("[data-asset-tile-id]")?.dataset.assetTileId;
                  const folderPath = target?.closest<HTMLElement>("[data-asset-folder-path]")
                    ?.dataset.assetFolderPath;
                  menuTargetRef.current = assetId
                    ? { kind: "asset", id: assetId }
                    : folderPath
                      ? { kind: "folder", path: folderPath }
                      : null;
                }}
              >
                <AssetPanelBody
                  assets={visibleAssets}
                  folders={folders}
                  currentFolder={currentFolder}
                  dropActive={dropActive}
                  creatingFolder={creatingFolder?.origin === "grid"}
                  creatingFolderName={creatingFolderName}
                  renamingFolder={renamingFolder}
                  renamingAsset={renamingAsset}
                  folderError={folderError}
                  assetDropTarget={assetDropTarget}
                  onOpenFolder={navigateTo}
                  onView={routeView}
                  onSelectAsset={selectAsset}
                  onSelectFolder={selectFolder}
                  onBeginDrag={beginDrag}
                  onDeleteAsset={deleteAsset}
                  onDeleteFolder={requestDeleteFolder}
                  onMoveAssets={moveAssets}
                  onMoveFolders={moveFolders}
                  onCommitNewFolder={commitNewFolderName}
                  onChangeNewFolderName={setCreatingFolderName}
                  onCancelNewFolder={cancelNewFolder}
                  onCommitRenameFolder={commitRenameFolderName}
                  onCancelRenameFolder={cancelRenameFolder}
                  onAssetDropTarget={setAssetDropTarget}
                  onClearFolderError={clearFolderError}
                  onRenameEnd={endRenameAsset}
                />
              </div>
            </ContextMenuTrigger>
            <ContextMenuContent
              className="min-w-40"
              onCloseAutoFocus={(event) => event.preventDefault()}
            >
              <GridContextMenuItems
                targetRef={menuTargetRef}
                visibleAssets={visibleAssets}
                renamingFolderGridPath={
                  renamingFolder?.origin === "grid" ? renamingFolder.path : null
                }
                nativeDialogOpen={nativeDialogOpen}
                onViewAsset={routeView}
                onInstantiate={onInstantiate}
                onRenameAsset={setRenamingAsset}
                onDeleteAsset={deleteAsset}
                onDeleteAssets={(targets) => void requestDeleteAssets(targets)}
                onRenameFolder={(folder) => startRenameFolder(folder, "grid")}
                onDeleteFolder={requestDeleteFolder}
                onNewFolder={onNewFolder}
                onImport={() => void onImportClick()}
              />
            </ContextMenuContent>
          </ContextMenu>
          <AssetDetailOverlay orientation={detailOrientation} />
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

/// The items of the grid's one shared context menu, mounted by Radix at open time
/// (closed content is unmounted), so each open reads the target ref and the live
/// selection fresh. Batch actions win over the tile under the pointer; an
/// unresolvable target falls through to the empty-area actions.
function GridContextMenuItems({
  targetRef,
  visibleAssets,
  renamingFolderGridPath,
  nativeDialogOpen,
  onViewAsset,
  onInstantiate,
  onRenameAsset,
  onDeleteAsset,
  onDeleteAssets,
  onRenameFolder,
  onDeleteFolder,
  onNewFolder,
  onImport,
}: {
  targetRef: React.RefObject<GridMenuTarget>;
  visibleAssets: AssetEntry[];
  /// The folder mid-rename on its grid tile (its input replaces the label), which
  /// redirects the menu to the empty-area actions.
  renamingFolderGridPath: string | null;
  nativeDialogOpen: boolean;
  onViewAsset(asset: AssetEntry): void;
  onInstantiate(modelId: string): void;
  onRenameAsset(assetId: string): void;
  onDeleteAsset(asset: AssetEntry): void;
  onDeleteAssets(assets: AssetEntry[]): void;
  onRenameFolder(folder: string): void;
  onDeleteFolder(folder: string): void;
  onNewFolder(): void;
  onImport(): void;
}) {
  // Live only while the menu is open; a selection emptied/filled between opens is
  // re-read on the next open.
  const selectedAssetIds = useEditorStore((s) => s.selectedAssetIds);
  const target = targetRef.current;
  const batchAssets = visibleAssets.filter((asset) => selectedAssetIds.has(asset.id));
  if (batchAssets.length > 0) {
    return (
      <>
        <ContextMenuItem
          onSelect={() => {
            for (const asset of batchAssets) {
              onViewAsset(asset);
            }
          }}
        >
          <Eye />
          View ({assetCountLabel(batchAssets.length)})
        </ContextMenuItem>
        {batchAssets.some((asset) => asset.type === "model") ? (
          <ContextMenuItem
            onSelect={() => {
              for (const asset of batchAssets) {
                if (asset.type === "model") {
                  onInstantiate(asset.id);
                }
              }
            }}
          >
            <Plus />
            Add to scene
          </ContextMenuItem>
        ) : null}
        <ContextMenuSeparator />
        <ContextMenuItem
          variant="destructive"
          className="bg-destructive/10 text-destructive focus:bg-destructive focus:text-destructive-foreground"
          onSelect={() => onDeleteAssets(batchAssets)}
        >
          <Trash />
          Delete ({assetCountLabel(batchAssets.length)})
        </ContextMenuItem>
      </>
    );
  }
  const asset =
    target?.kind === "asset" ? visibleAssets.find((entry) => entry.id === target.id) : undefined;
  if (asset) {
    return (
      <>
        <ContextMenuItem onSelect={() => onViewAsset(asset)}>
          <Eye />
          View
        </ContextMenuItem>
        {asset.type === "model" ? (
          <ContextMenuItem onSelect={() => onInstantiate(asset.id)}>
            <Plus />
            Add to scene
          </ContextMenuItem>
        ) : null}
        <ContextMenuItem onSelect={() => onRenameAsset(asset.id)}>
          <Pencil />
          Rename
        </ContextMenuItem>
        <ContextMenuSeparator />
        <ContextMenuItem
          variant="destructive"
          className="bg-destructive/10 text-destructive focus:bg-destructive focus:text-destructive-foreground"
          onSelect={() => onDeleteAsset(asset)}
        >
          <Trash />
          Delete
        </ContextMenuItem>
      </>
    );
  }
  if (target?.kind === "folder" && target.path !== renamingFolderGridPath) {
    const folder = target.path;
    return (
      <>
        <ContextMenuItem onSelect={() => onRenameFolder(folder)}>
          <Pen />
          Rename
        </ContextMenuItem>
        <ContextMenuSeparator />
        <ContextMenuItem
          variant="destructive"
          className="bg-destructive/10 text-destructive focus:bg-destructive focus:text-destructive-foreground"
          onSelect={() => onDeleteFolder(folder)}
        >
          <Trash />
          Delete
        </ContextMenuItem>
      </>
    );
  }
  return (
    <>
      <ContextMenuItem onSelect={onNewFolder}>
        <Folder />
        New Folder
      </ContextMenuItem>
      <ContextMenuItem onSelect={onImport} disabled={nativeDialogOpen}>
        <Plus />
        Import
      </ContextMenuItem>
    </>
  );
}

/// The details overlay for a single selected asset, isolated so its open/close and
/// the probe-asset round trip never render the grid. The selector collapses the
/// selection to a primitive (the lone asset id or null), and the marquee gate keeps
/// it closed while a sweep flips the selection through "exactly one" — it opens
/// once, on release.
function AssetDetailOverlay({ orientation }: { orientation: "right" | "bottom" }) {
  logRender("AssetDetailOverlay");
  const detailAssetId = useEditorStore((s) =>
    !s.assetMarqueeActive && s.selectedAssetIds.size === 1 && s.selectedFolderPaths.size === 0
      ? ([...s.selectedAssetIds][0] ?? null)
      : null,
  );
  const setAssetSelection = useEditorStore((s) => s.setAssetSelection);
  const [metadata, setMetadata] = useState<AssetMetadataDto | null>(null);

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

  return (
    <AssetMetadataPanel
      metadata={metadata}
      open={detailAssetId !== null}
      orientation={orientation}
      onClose={() => setAssetSelection([], [])}
    />
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

/// memo'd with every callback prop stable (the panel hoists them through
/// useCallback), so panel renders that change nothing for the grid skip it.
const AssetPanelBody = memo(function AssetPanelBody({
  assets,
  folders,
  currentFolder,
  dropActive,
  creatingFolder,
  creatingFolderName,
  renamingFolder,
  renamingAsset,
  folderError,
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
  onCommitNewFolder,
  onChangeNewFolderName,
  onCancelNewFolder,
  onCommitRenameFolder,
  onCancelRenameFolder,
  onAssetDropTarget,
  onClearFolderError,
  onRenameEnd,
}: {
  assets: AssetEntry[];
  folders: string[];
  currentFolder: string | null;
  dropActive: boolean;
  creatingFolder: boolean;
  creatingFolderName: string;
  renamingFolder: FolderActionTarget | null;
  renamingAsset: string | null;
  folderError: string | null;
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
  onCommitNewFolder(name: string): void;
  onChangeNewFolderName(name: string): void;
  onCancelNewFolder(): void;
  onCommitRenameFolder(folder: string, name: string): void;
  onCancelRenameFolder(): void;
  onAssetDropTarget(folder: string | null): void;
  onClearFolderError(): void;
  onRenameEnd(): void;
}) {
  logRender("AssetPanelBody");
  const folderItems = sortedFolderItems(folders, currentFolder, creatingFolder);
  const parentFolder = currentFolder !== null ? parentFolderPath(currentFolder) : null;
  const hasParentTile = currentFolder !== null;
  const blank =
    !hasParentTile && !creatingFolder && folderItems.length === 0 && assets.length === 0;
  const empty = blank && !currentFolder;
  const folderEmpty = blank && currentFolder !== null;
  const panelRef = useRef<HTMLDivElement | null>(null);
  const boxRef = useRef<HTMLDivElement | null>(null);
  const marqueeRef = useRef<MarqueeDrag | null>(null);
  const setAssetSelection = useEditorStore((s) => s.setAssetSelection);
  const setAssetMarqueeActive = useEditorStore((s) => s.setAssetMarqueeActive);
  const marqueeActive = useEditorStore((s) => s.assetMarqueeActive);

  // Per-kind bindings of the panel's beginDrag, made once so every tile of a kind
  // shares one identity.
  const beginAssetDrag = useCallback(
    (entry: AssetEntry, event: ReactDragEvent): void => onBeginDrag("asset", entry.id, event),
    [onBeginDrag],
  );
  const beginFolderDrag = useCallback(
    (path: string, event: ReactDragEvent): void => onBeginDrag("folder", path, event),
    [onBeginDrag],
  );

  // Snapshot every tile's client rect in one pass. Taken at drag start (and again
  // if the grid scrolls mid-drag) so the per-frame hit test never reads layout.
  const snapshotMarqueeTiles = (drag: MarqueeDrag): void => {
    const panel = panelRef.current;
    if (!panel) {
      return;
    }
    drag.scrollTop = drag.viewport?.scrollTop ?? 0;
    drag.tiles = [];
    for (const el of panel.querySelectorAll<HTMLElement>("[data-asset-tile-id]")) {
      drag.tiles.push({
        kind: "asset",
        key: el.dataset.assetTileId ?? "",
        rect: el.getBoundingClientRect(),
      });
    }
    for (const el of panel.querySelectorAll<HTMLElement>("[data-asset-folder-path]")) {
      drag.tiles.push({
        kind: "folder",
        key: el.dataset.assetFolderPath ?? "",
        rect: el.getBoundingClientRect(),
      });
    }
  };

  // The per-frame marquee step: position the box via direct style writes (no
  // re-render) and hit-test against the cached rects, propagating the selection
  // only when the hit set actually changed. Runs at most once per animation frame
  // regardless of the pointer's event rate.
  const applyMarquee = (): void => {
    const drag = marqueeRef.current;
    if (!drag) {
      return;
    }
    drag.raf = 0;
    if (drag.viewport && drag.viewport.scrollTop !== drag.scrollTop) {
      snapshotMarqueeTiles(drag);
    }
    const rect = marqueeRect(drag);
    const box = boxRef.current;
    if (box) {
      box.style.left = `${rect.left - drag.panelLeft}px`;
      box.style.top = `${rect.top - drag.panelTop}px`;
      box.style.width = `${rect.right - rect.left}px`;
      box.style.height = `${rect.bottom - rect.top}px`;
    }
    const assetIds: string[] = [];
    const folderPaths: string[] = [];
    for (const tile of drag.tiles) {
      if (rectsIntersect(rect, tile.rect)) {
        (tile.kind === "asset" ? assetIds : folderPaths).push(tile.key);
      }
    }
    const hits = `${assetIds.join("\n")}\0${folderPaths.join("\n")}`;
    if (hits !== drag.lastHits) {
      drag.lastHits = hits;
      setAssetSelection(assetIds, folderPaths);
    }
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
    const panelRect = event.currentTarget.getBoundingClientRect();
    const drag: MarqueeDrag = {
      startX: event.clientX,
      startY: event.clientY,
      currentX: event.clientX,
      currentY: event.clientY,
      panelLeft: panelRect.left,
      panelTop: panelRect.top,
      viewport: event.currentTarget.querySelector("[data-radix-scroll-area-viewport]"),
      scrollTop: 0,
      tiles: [],
      lastHits: "\0",
      raf: 0,
    };
    snapshotMarqueeTiles(drag);
    marqueeRef.current = drag;
    setAssetMarqueeActive(true);
    setAssetSelection([], []);
  };

  const moveMarquee = (event: ReactPointerEvent<HTMLDivElement>): void => {
    const drag = marqueeRef.current;
    if (!drag) {
      return;
    }
    drag.currentX = event.clientX;
    drag.currentY = event.clientY;
    if (drag.raf === 0) {
      drag.raf = requestAnimationFrame(applyMarquee);
    }
  };

  const endMarquee = (event: ReactPointerEvent<HTMLDivElement>): void => {
    const drag = marqueeRef.current;
    if (!drag) {
      return;
    }
    if (drag.raf !== 0) {
      cancelAnimationFrame(drag.raf);
      drag.raf = 0;
    }
    // Flush the final position so a fast flick-release still selects what the
    // pointer covered.
    applyMarquee();
    marqueeRef.current = null;
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
    setAssetMarqueeActive(false);
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
        // dragleave bubbles on every interior element boundary, and the WebKitGTK
        // webview leaves relatedTarget null, so a containment check clears the
        // drop-target on each micro-move and the folder highlight flickers. dragover
        // already keeps the target current while inside, so clear only once the
        // pointer is genuinely outside the panel bounds.
        const rect = event.currentTarget.getBoundingClientRect();
        if (
          event.clientX >= rect.left &&
          event.clientX < rect.right &&
          event.clientY >= rect.top &&
          event.clientY < rect.bottom
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
              {hasParentTile ? (
                <ParentFolderTile
                  target={parentFolder}
                  onOpen={onOpenFolder}
                  onMoveAssets={onMoveAssets}
                  onMoveFolders={onMoveFolders}
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
                    path={item.path}
                    name={item.label}
                    editing={renamingFolder?.origin === "grid" && renamingFolder.path === item.path}
                    invalid={folderError !== null && renamingFolder?.path === item.path}
                    dragActive={assetDropTarget === item.path}
                    onOpen={onOpenFolder}
                    onSelect={onSelectFolder}
                    onBeginDrag={beginFolderDrag}
                    onAssetDropTarget={onAssetDropTarget}
                    onMoveAssets={onMoveAssets}
                    onMoveFolders={onMoveFolders}
                    onDelete={onDeleteFolder}
                    onCommitRename={onCommitRenameFolder}
                    onChangeRename={onClearFolderError}
                    onCancelRename={onCancelRenameFolder}
                  />
                ),
              )}
              {assets.map((asset) => (
                <AssetTile
                  key={asset.id}
                  entry={asset}
                  renaming={renamingAsset === asset.id}
                  onView={onView}
                  onDelete={onDeleteAsset}
                  onSelect={onSelectAsset}
                  onBeginDrag={beginAssetDrag}
                  onRenameEnd={onRenameEnd}
                />
              ))}
            </div>
          )}
        </div>
      </ScrollArea>
      {marqueeActive ? (
        <div ref={boxRef} className="pointer-events-none absolute border border-ring bg-ring/15" />
      ) : null}
    </div>
  );
});

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
      <div className="flex aspect-square w-full items-center justify-center">
        <Folder className="size-16 fill-current stroke-current text-yellow-500" />
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

const ParentFolderTile = memo(function ParentFolderTile({
  target,
  onOpen,
  onMoveAssets,
  onMoveFolders,
}: {
  target: string | null;
  onOpen(folder: string | null): void;
  onMoveAssets(assetIds: string[], folder: string | null): void;
  onMoveFolders(paths: string[], parent: string | null): void;
}) {
  logRender("ParentFolderTile");
  const [dragActive, setDragActive] = useState(false);
  return (
    <div className="relative w-[72px]">
      <button
        type="button"
        data-asset-parent-folder="true"
        className={cn(
          "flex w-[72px] flex-col gap-1 rounded-md border border-transparent p-1 text-left transition-colors hover:border-ring hover:bg-accent/40",
          dragActive && "border-ring bg-accent/60 ring-1 ring-ring",
        )}
        onClick={() => onOpen(target)}
        onDragEnter={(event) => {
          if (isCatalogDrag(event.dataTransfer)) {
            setDragActive(true);
          }
        }}
        onDragOver={(event) => {
          if (isCatalogDrag(event.dataTransfer)) {
            event.preventDefault();
            event.dataTransfer.dropEffect = "move";
            setDragActive(true);
          }
        }}
        onDragLeave={() => setDragActive(false)}
        onDrop={(event) => {
          const ids = assetIdsFromPayload(readAssetPayload(event.dataTransfer));
          const folderPaths = readFolderPayload(event.dataTransfer);
          if (ids.length === 0 && folderPaths.length === 0) {
            return;
          }
          event.preventDefault();
          event.stopPropagation();
          setDragActive(false);
          if (folderPaths.length > 0) {
            onMoveFolders(folderPaths, target);
          }
          if (ids.length > 0) {
            onMoveAssets(ids, target);
          }
        }}
      >
        <div className="flex aspect-square w-full items-center justify-center">
          <Folder className="size-16 fill-current stroke-current text-yellow-500" />
        </div>
        <span className="truncate px-0.5 text-center font-mono text-[11px] leading-tight text-foreground">
          ../
        </span>
      </button>
    </div>
  );
});

/// memo'd like AssetTile: the body passes one function identity per callback to
/// every folder tile (the tile binds its own `path` at call time).
const FolderTile = memo(function FolderTile({
  path,
  name,
  editing = false,
  invalid = false,
  dragActive = false,
  onOpen,
  onSelect,
  onBeginDrag,
  onAssetDropTarget,
  onMoveAssets,
  onMoveFolders,
  onDelete,
  onCommitRename,
  onChangeRename,
  onCancelRename,
}: {
  path: string;
  name: string;
  editing?: boolean;
  invalid?: boolean;
  dragActive?: boolean;
  onOpen(path: string): void;
  onSelect?(path: string, event: ReactMouseEvent): void;
  onBeginDrag(path: string, event: ReactDragEvent): void;
  /// Hover highlight for a catalog drag: enter/over report this tile's path,
  /// drop/drag-end clear with null.
  onAssetDropTarget(path: string | null): void;
  onMoveAssets(assetIds: string[], path: string): void;
  onMoveFolders?(paths: string[], path: string): void;
  onDelete?(path: string): void;
  onCommitRename?(path: string, name: string): void;
  onChangeRename?(): void;
  onCancelRename?(): void;
}) {
  logRender("FolderTile");
  const selected = useEditorStore((s) => s.selectedFolderPaths.has(path));
  const content = (
    <>
      <div className="flex aspect-square w-full items-center justify-center">
        <Folder className="size-16 fill-current stroke-current text-yellow-500" />
      </div>
      {editing && onCommitRename && onCancelRename ? (
        <FolderNameInput
          initial={name}
          invalid={invalid}
          onChange={onChangeRename}
          onCommit={(next) => onCommitRename(path, next)}
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
        "flex w-[72px] flex-col gap-1 rounded-md border border-transparent p-1 text-left transition-colors hover:border-ring hover:bg-accent/40",
        selected && "border-ring bg-accent/60 ring-1 ring-ring",
        dragActive && "border-ring bg-accent/60 ring-1 ring-ring",
      )}
      draggable={!editing}
      onClick={(event) => onSelect?.(path, event)}
      onDoubleClick={() => onOpen(path)}
      // The configured delete key on the focused tile runs the same delete as the
      // context menu.
      onKeyDown={(event) => {
        if (
          !editing &&
          onDelete &&
          matchesBinding(event, "assets.delete", useEditorStore.getState().keyBindings)
        ) {
          event.preventDefault();
          onDelete(path);
        }
      }}
      onDragStart={(event) => onBeginDrag(path, event)}
      onDragEnter={(event) => {
        if (isCatalogDrag(event.dataTransfer)) {
          onAssetDropTarget(path);
        }
      }}
      onDragOver={(event) => {
        if (isCatalogDrag(event.dataTransfer)) {
          event.preventDefault();
          event.dataTransfer.dropEffect = "move";
          onAssetDropTarget(path);
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
        onAssetDropTarget(null);
        if (folderPaths.length > 0) {
          onMoveFolders?.(folderPaths, path);
        }
        if (ids.length > 0) {
          onMoveAssets(ids, path);
        }
      }}
      onDragEnd={() => onAssetDropTarget(null)}
    >
      {content}
    </button>
  );

  return <div className="relative w-[72px]">{tile}</div>;
});

/// Usage lines shown in the delete dialog before collapsing into "And X additional".
const MAX_USAGE_LINES = 5;

interface PendingAssetDelete {
  assets: AssetEntry[];
  usages: AssetUsageDto[];
}

/// Drag-local marquee state, kept in a ref (NOT React state): the box position is
/// written straight to the DOM and the hit test runs against rects cached at drag
/// start, so a pointer move never renders anything by itself — only an actual
/// change in the hit set reaches the store via setAssetSelection.
interface MarqueeDrag {
  startX: number;
  startY: number;
  currentX: number;
  currentY: number;
  panelLeft: number;
  panelTop: number;
  viewport: HTMLElement | null;
  scrollTop: number;
  tiles: { kind: "asset" | "folder"; key: string; rect: RectLike }[];
  lastHits: string;
  raf: number;
}

interface RectLike {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

function marqueeRect(marquee: MarqueeDrag): RectLike {
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
