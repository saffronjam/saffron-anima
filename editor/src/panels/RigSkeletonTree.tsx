/// The rig editor's left panel: the rig's bone hierarchy as a tree (the home bones never had in the
/// scene outliner). Read-only navigation — expand/collapse and select a bone; selecting drives the
/// preview overlay's highlight channel (set-skeleton-highlight, wired in RigEditorWorkspace), never
/// scene selection, so the selection-keyed animation state the timeline reads stays alive. Bone names
/// render verbatim (they are the durable clip-binding keys); render joints are emphasized, intermediate
/// nodes muted. Follows HierarchyTree's row/indent idiom so the two trees read as siblings.
import { useMemo, useState } from "react";
import { Bone, ChevronDown, ChevronRight } from "lucide-react";
import { ScrollArea } from "@/components/ui/scroll-area";
import { cn } from "@/lib/utils";
import type { RigBoneDto } from "../protocol";

/// Indent caps at this depth so a deep humanoid chain never forces horizontal scroll.
const MAX_INDENT_DEPTH = 10;
const INDENT_PX = 12;

interface RigSkeletonTreeProps {
  bones: RigBoneDto[];
  /// The get-rig node index currently highlighted (-1 = none). Local view state in the workspace.
  selectedIndex: number;
  onSelect: (joint: number) => void;
}

export function RigSkeletonTree({ bones, selectedIndex, onSelect }: RigSkeletonTreeProps) {
  // children-by-parent (keyed by node index) + roots, rebuilt only when the bone list changes.
  const { childrenOf, roots } = useMemo(() => {
    const present = new Set(bones.map((b) => b.index));
    const byParent = new Map<number, RigBoneDto[]>();
    const rootList: RigBoneDto[] = [];
    for (const bone of bones) {
      if (bone.parent >= 0 && present.has(bone.parent)) {
        const list = byParent.get(bone.parent);
        if (list) {
          list.push(bone);
        } else {
          byParent.set(bone.parent, [bone]);
        }
      } else {
        rootList.push(bone);
      }
    }
    return { childrenOf: byParent, roots: rootList };
  }, [bones]);

  const [collapsed, setCollapsed] = useState<ReadonlySet<number>>(() => new Set());

  if (bones.length === 0) {
    return (
      <div className="flex h-full items-center justify-center bg-background p-3 text-center text-xs italic text-muted-foreground">
        No skeleton in this asset.
      </div>
    );
  }

  const toggle = (index: number): void => {
    setCollapsed((prev) => {
      const next = new Set(prev);
      if (next.has(index)) {
        next.delete(index);
      } else {
        next.add(index);
      }
      return next;
    });
  };

  const renderRows = (bone: RigBoneDto, depth: number): React.ReactNode => {
    const kids = childrenOf.get(bone.index) ?? [];
    const hasKids = kids.length > 0;
    const isCollapsed = collapsed.has(bone.index);
    const selected = selectedIndex === bone.index;
    return (
      <div key={bone.index}>
        <div
          role="button"
          tabIndex={0}
          className={cn(
            "flex w-full cursor-default items-center gap-1 py-0.5 pr-2 text-xs",
            selected ? "bg-accent text-accent-foreground" : "hover:bg-accent/40",
            bone.joint ? "text-foreground" : "text-muted-foreground",
          )}
          style={{ paddingLeft: `${Math.min(depth, MAX_INDENT_DEPTH) * INDENT_PX + 8}px` }}
          onClick={() => onSelect(bone.index)}
          onKeyDown={(e) => {
            if (e.key === "Enter" || e.key === " ") {
              e.preventDefault();
              onSelect(bone.index);
            }
          }}
        >
          {hasKids ? (
            <span
              className="flex size-4 shrink-0 items-center justify-center text-muted-foreground"
              onClick={(e) => {
                e.stopPropagation();
                toggle(bone.index);
              }}
            >
              {isCollapsed ? (
                <ChevronRight className="size-3" />
              ) : (
                <ChevronDown className="size-3" />
              )}
            </span>
          ) : (
            <span className="size-4 shrink-0" />
          )}
          {bone.joint && <Bone className="size-3 shrink-0 opacity-70" />}
          <span className="truncate">{bone.name || `node ${bone.index}`}</span>
        </div>
        {hasKids && !isCollapsed && kids.map((kid) => renderRows(kid, depth + 1))}
      </div>
    );
  };

  return (
    <div className="flex h-full flex-col bg-background">
      <div className="border-b border-border px-3 py-1.5 text-xs font-medium uppercase tracking-wide text-muted-foreground">
        Skeleton
      </div>
      <ScrollArea className="min-h-0 flex-1">
        <div className="py-1">{roots.map((bone) => renderRows(bone, 0))}</div>
      </ScrollArea>
    </div>
  );
}
