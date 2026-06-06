/// The folder sidebar of the Assets panel: a pinned Root row plus the virtual-folder
/// forest built client-side from the flat `assetFolders` path list. Rows navigate on
/// click, expand via the twisty, accept asset drops (`move-asset`), and carry the
/// same New Folder / Rename / Delete commands as the grid tiles. Expand state is
/// local, with an ancestor-reveal effect so tile, breadcrumb, and history navigation
/// never lands inside a collapsed branch.
import { useEffect, useMemo, useRef, useState } from "react";
import { ChevronRight, Folder, FolderPlus, Pen, Trash } from "lucide-react";
import {
  ASSET_DND_MIME,
  DeleteConfirm,
  assetIdsFromPayload,
  readAssetPayload,
} from "../components/AssetTile";
import { Input } from "@/components/ui/input";
import { cn } from "@/lib/utils";
import {
  ContextMenu,
  ContextMenuContent,
  ContextMenuItem,
  ContextMenuSeparator,
  ContextMenuTrigger,
} from "@/components/ui/context-menu";

/// One node of the virtual-folder forest; `path` is the full slash-joined path.
export interface FolderNode {
  path: string;
  name: string;
  children: FolderNode[];
}

/// The last path segment of a folder path.
export function folderLabel(folder: string): string {
  const slash = folder.lastIndexOf("/");
  return slash >= 0 ? folder.slice(slash + 1) : folder;
}

/// Every prefix path of `folder`, shortest first, including `folder` itself.
export function folderAncestorPaths(folder: string): string[] {
  const segments = folder.split("/");
  const paths: string[] = [];
  for (let i = 1; i <= segments.length; i += 1) {
    paths.push(segments.slice(0, i).join("/"));
  }
  return paths;
}

/// Build the sorted folder forest from the flat path list, synthesizing any
/// intermediate node the list does not carry explicitly.
export function buildFolderTree(folders: string[]): FolderNode[] {
  const roots: FolderNode[] = [];
  const byPath = new Map<string, FolderNode>();
  const nodeFor = (path: string): FolderNode => {
    const existing = byPath.get(path);
    if (existing) {
      return existing;
    }
    const node: FolderNode = { path, name: folderLabel(path), children: [] };
    byPath.set(path, node);
    const slash = path.lastIndexOf("/");
    if (slash >= 0) {
      nodeFor(path.slice(0, slash)).children.push(node);
    } else {
      roots.push(node);
    }
    return node;
  };
  for (const folder of folders) {
    nodeFor(folder);
  }
  const sortBranch = (nodes: FolderNode[]): void => {
    nodes.sort((a, b) =>
      a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: "base" }),
    );
    for (const node of nodes) {
      sortBranch(node.children);
    }
  };
  sortBranch(roots);
  return roots;
}

export interface FolderTreeProps {
  folders: string[];
  currentFolder: string | null;
  /// The folder being renamed inline in the TREE (the panel keeps grid-initiated
  /// renames on the grid tile instead).
  renamingFolder: string | null;
  renameInvalid: boolean;
  /// The tree-initiated delete awaiting confirmation, with its prepared body text.
  pendingDelete: { path: string; body: string } | null;
  onNavigate(folder: string | null): void;
  onMoveAssets(assetIds: string[], folder: string | null): void;
  onNewFolder(parent: string | null): void;
  onStartRename(folder: string): void;
  onChangeRename(): void;
  onCommitRename(folder: string, name: string): void;
  onCancelRename(): void;
  onDelete(folder: string): void;
  onConfirmDelete(folder: string): void;
  onCancelDelete(): void;
}

export function AssetFolderTree(props: FolderTreeProps) {
  const { folders, currentFolder } = props;
  const roots = useMemo(() => buildFolderTree(folders), [folders]);
  const [expanded, setExpanded] = useState<Set<string>>(() => new Set());
  // The hovered asset-drop row, by folder path.
  const [dropTarget, setDropTarget] = useState<string | null>(null);

  // Reveal externally driven navigation (grid tiles, breadcrumbs, back/forward):
  // expand the current folder and every ancestor.
  useEffect(() => {
    if (!currentFolder) {
      return;
    }
    setExpanded((current) => {
      const next = new Set(current);
      for (const path of folderAncestorPaths(currentFolder)) {
        next.add(path);
      }
      return next.size === current.size ? current : next;
    });
  }, [currentFolder]);

  const toggleExpanded = (path: string): void => {
    setExpanded((current) => {
      const next = new Set(current);
      if (next.has(path)) {
        next.delete(path);
      } else {
        next.add(path);
      }
      return next;
    });
  };

  const tree: TreeContext = {
    ...props,
    expanded,
    toggleExpanded,
    dropTarget,
    setDropTarget,
  };

  return (
    <div className="p-1" role="tree" aria-label="Asset folders">
      {roots.map((node) => (
        <FolderRow key={node.path} node={node} depth={0} tree={tree} />
      ))}
    </div>
  );
}

interface TreeContext extends FolderTreeProps {
  expanded: Set<string>;
  toggleExpanded(path: string): void;
  dropTarget: string | null;
  setDropTarget(target: string | null): void;
}

/// Drag-drop handlers making a row an asset-move target; `key` is the row's
/// highlight identity and `folder` the move destination.
function dropHandlers(tree: TreeContext, key: string, folder: string | null) {
  return {
    onDragEnter: (event: React.DragEvent): void => {
      if (event.dataTransfer.types.includes(ASSET_DND_MIME)) {
        tree.setDropTarget(key);
      }
    },
    onDragOver: (event: React.DragEvent): void => {
      if (event.dataTransfer.types.includes(ASSET_DND_MIME)) {
        event.preventDefault();
        event.dataTransfer.dropEffect = "move";
        tree.setDropTarget(key);
      }
    },
    onDragLeave: (): void => {
      if (tree.dropTarget === key) {
        tree.setDropTarget(null);
      }
    },
    onDrop: (event: React.DragEvent): void => {
      const ids = assetIdsFromPayload(readAssetPayload(event.dataTransfer));
      if (ids.length === 0) {
        return;
      }
      event.preventDefault();
      event.stopPropagation();
      tree.setDropTarget(null);
      tree.onMoveAssets(ids, folder);
    },
  };
}

const rowClass = "flex w-full items-center gap-1 truncate rounded-md px-1 py-1 text-left text-xs";

function rowStateClass(selected: boolean, dropActive: boolean): string {
  return cn(
    rowClass,
    selected ? "bg-primary text-primary-foreground" : "text-foreground hover:bg-accent",
    dropActive && "ring-1 ring-ring bg-accent/60",
  );
}

function FolderRow({ node, depth, tree }: { node: FolderNode; depth: number; tree: TreeContext }) {
  const selected = tree.currentFolder === node.path;
  const expanded = tree.expanded.has(node.path);
  const hasChildren = node.children.length > 0;
  const renaming = tree.renamingFolder === node.path;
  const confirmingDelete = tree.pendingDelete?.path === node.path;

  const row = renaming ? (
    <div className={rowClass} style={{ paddingLeft: depth * 14 + 4 }}>
      <Folder className="size-3.5 flex-none opacity-70" />
      <FolderRenameInput
        initial={node.name}
        invalid={tree.renameInvalid}
        onChange={tree.onChangeRename}
        onCommit={(name) => tree.onCommitRename(node.path, name)}
        onCancel={tree.onCancelRename}
      />
    </div>
  ) : (
    <button
      type="button"
      role="treeitem"
      aria-selected={selected}
      aria-expanded={hasChildren ? expanded : undefined}
      className={rowStateClass(selected, tree.dropTarget === node.path)}
      style={{ paddingLeft: depth * 14 + 4 }}
      title={node.path}
      onClick={() => tree.onNavigate(node.path)}
      {...dropHandlers(tree, node.path, node.path)}
    >
      {hasChildren ? (
        <span
          role="button"
          aria-label={expanded ? "Collapse" : "Expand"}
          className="flex size-4 flex-none items-center justify-center rounded hover:bg-foreground/10"
          onClick={(event) => {
            event.stopPropagation();
            tree.toggleExpanded(node.path);
          }}
        >
          <ChevronRight className={cn("size-3.5 transition-transform", expanded && "rotate-90")} />
        </span>
      ) : (
        <span className="size-4 flex-none" />
      )}
      <Folder className="size-3.5 flex-none opacity-70" />
      <span className="truncate">{node.name}</span>
    </button>
  );

  return (
    <>
      <div className="relative" onContextMenu={(event) => event.stopPropagation()}>
        <ContextMenu>
          <ContextMenuTrigger asChild>{row}</ContextMenuTrigger>
          <ContextMenuContent className="min-w-36">
            <ContextMenuItem onSelect={() => tree.onNewFolder(node.path)}>
              <FolderPlus />
              New Folder
            </ContextMenuItem>
            <ContextMenuItem onSelect={() => tree.onStartRename(node.path)}>
              <Pen />
              Rename
            </ContextMenuItem>
            <ContextMenuSeparator />
            <ContextMenuItem
              variant="destructive"
              className="bg-destructive/10 text-destructive focus:bg-destructive focus:text-destructive-foreground"
              onSelect={() => tree.onDelete(node.path)}
            >
              <Trash />
              Delete
            </ContextMenuItem>
          </ContextMenuContent>
        </ContextMenu>
        {confirmingDelete && tree.pendingDelete ? (
          <DeleteConfirm
            title={`Delete ${node.name}?`}
            body={tree.pendingDelete.body}
            onConfirm={() => tree.onConfirmDelete(node.path)}
            onCancel={tree.onCancelDelete}
          />
        ) : null}
      </div>
      {hasChildren && expanded
        ? node.children.map((child) => (
            <FolderRow key={child.path} node={child} depth={depth + 1} tree={tree} />
          ))
        : null}
    </>
  );
}

/// Inline rename input rendered in place of a folder row: autofocus + select-all,
/// Enter/blur commits once (the `settled` ref absorbs the unmount blur), Escape
/// cancels.
function FolderRenameInput({
  initial,
  invalid,
  onChange,
  onCommit,
  onCancel,
}: {
  initial: string;
  invalid: boolean;
  onChange(): void;
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
        "h-5 min-w-0 flex-1 rounded-sm px-1 py-0 font-mono text-[11px]",
        invalid && "border-destructive ring-1 ring-destructive",
      )}
      onClick={(event) => event.stopPropagation()}
      onChange={(event) => {
        setValue(event.currentTarget.value);
        onChange();
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
