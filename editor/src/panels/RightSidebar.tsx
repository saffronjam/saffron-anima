/// The right sidebar: the performance tools (Stats / Profiler) opened from the Topbar wrench
/// menu, shown as a compact strip of closeable tabs (shorter than the main view tabs). Closing
/// the last tab empties `rightTools` and Layout removes the sidebar entirely, so the viewport
/// reclaims the width.
import { X } from "lucide-react";
import { useEditorStore, type RightTool } from "../state/store";
import { RenderStatsPanel } from "./RenderStatsPanel";
import { ProfilerPanel } from "./ProfilerPanel";
import { MaterialEditorPanel } from "./MaterialEditorPanel";
import { cn } from "@/lib/utils";

const TOOL_LABEL: Record<RightTool, string> = {
  stats: "Stats",
  profiler: "Profiler",
  material: "Material",
};

export function RightSidebar() {
  const rightTools = useEditorStore((s) => s.rightTools);
  const activeRightTool = useEditorStore((s) => s.activeRightTool);
  const setActiveRightTool = useEditorStore((s) => s.setActiveRightTool);
  const closeRightTool = useEditorStore((s) => s.closeRightTool);

  const active = activeRightTool ?? rightTools[0] ?? null;

  return (
    <div className="flex h-full min-h-0 flex-col border-l border-border">
      <div
        className="flex h-8 flex-none items-center border-b border-border bg-background"
        role="tablist"
      >
        {rightTools.map((tool) => {
          const selected = tool === active;
          return (
            <div
              key={tool}
              role="tab"
              aria-selected={selected}
              tabIndex={0}
              onClick={() => setActiveRightTool(tool)}
              onKeyDown={(event) => {
                if (event.key === "Enter" || event.key === " ") {
                  event.preventDefault();
                  setActiveRightTool(tool);
                }
              }}
              className={cn(
                "-mb-px flex h-8 cursor-pointer select-none items-center gap-1 border-b-2 pl-2.5 pr-1 text-xs",
                selected
                  ? "border-primary text-foreground"
                  : "border-transparent text-muted-foreground hover:text-foreground",
              )}
            >
              {TOOL_LABEL[tool]}
              <button
                type="button"
                aria-label={`Close ${TOOL_LABEL[tool]}`}
                onClick={(event) => {
                  event.stopPropagation();
                  closeRightTool(tool);
                }}
                className="rounded-sm p-0.5 text-muted-foreground opacity-60 hover:bg-muted hover:opacity-100"
              >
                <X className="size-3" />
              </button>
            </div>
          );
        })}
      </div>
      {/* Each opened tool stays mounted and is hidden (display:none) when not active, so the
          Material panel keeps its selected material + preview across tool switches instead of
          remounting and re-fetching. None of these panels self-polls (metrics polling lives in
          store.ts keyed on rightTools, not on which tool is active), so hiding is cost-free. */}
      <div className="min-h-0 flex-1">
        {rightTools.includes("stats") && (
          <div className={cn("h-full", active !== "stats" && "hidden")}>
            <RenderStatsPanel />
          </div>
        )}
        {rightTools.includes("profiler") && (
          <div className={cn("h-full", active !== "profiler" && "hidden")}>
            <ProfilerPanel />
          </div>
        )}
        {rightTools.includes("material") && (
          <div className={cn("h-full", active !== "material" && "hidden")}>
            <MaterialEditorPanel />
          </div>
        )}
      </div>
    </div>
  );
}
