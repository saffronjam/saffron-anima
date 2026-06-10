/// The bottom dock: the tools (currently just the Timeline) opened from the Topbar, shown
/// as a compact strip of closeable tabs like the right sidebar. Closing the last tab empties
/// `bottomTools` and Layout removes the dock entirely, so the viewport reclaims the height.
import { X } from "lucide-react";
import { useEditorStore, type BottomTool } from "../state/store";
import { TimelinePanel } from "./TimelinePanel";
import { cn } from "@/lib/utils";

const BOTTOM_TOOL_LABEL: Record<BottomTool, string> = { timeline: "Timeline" };

export function BottomDock() {
  const bottomTools = useEditorStore((s) => s.bottomTools);
  const activeBottomTool = useEditorStore((s) => s.activeBottomTool);
  const setActiveBottomTool = useEditorStore((s) => s.setActiveBottomTool);
  const closeBottomTool = useEditorStore((s) => s.closeBottomTool);

  const active = activeBottomTool ?? bottomTools[0] ?? null;

  return (
    <div className="flex h-full min-h-0 flex-col border-t border-border bg-background">
      <div
        className="flex h-8 flex-none items-center border-b border-border bg-background"
        role="tablist"
      >
        {bottomTools.map((tool) => {
          const selected = tool === active;
          return (
            <div
              key={tool}
              role="tab"
              aria-selected={selected}
              tabIndex={0}
              onClick={() => setActiveBottomTool(tool)}
              onKeyDown={(event) => {
                if (event.key === "Enter" || event.key === " ") {
                  event.preventDefault();
                  setActiveBottomTool(tool);
                }
              }}
              className={cn(
                "-mb-px flex h-8 cursor-pointer select-none items-center gap-1 border-b-2 pl-2.5 pr-1 text-xs",
                selected
                  ? "border-primary text-foreground"
                  : "border-transparent text-muted-foreground hover:text-foreground",
              )}
            >
              {BOTTOM_TOOL_LABEL[tool]}
              <button
                type="button"
                aria-label={`Close ${BOTTOM_TOOL_LABEL[tool]}`}
                onClick={(event) => {
                  event.stopPropagation();
                  closeBottomTool(tool);
                }}
                className="rounded-sm p-0.5 text-muted-foreground opacity-60 hover:bg-muted hover:opacity-100"
              >
                <X className="size-3" />
              </button>
            </div>
          );
        })}
      </div>
      <div className="min-h-0 flex-1">{active === "timeline" ? <TimelinePanel /> : null}</div>
    </div>
  );
}
