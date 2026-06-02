/// The topbar gizmo group: T/R/S operation buttons + a world/local space toggle,
/// wired to the engine's single gizmo state (`set-gizmo`/`get-gizmo`). Clicks set
/// `store.gizmo` optimistically and fire `set-gizmo`; the reconcile poll's
/// `get-gizmo` read keeps it in sync with external mutations (e.g. `se set-gizmo`).
import { Move3D, Rotate3D, Scaling } from "lucide-react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import type { GizmoState } from "../protocol";
import { Button } from "@/components/ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";

type GizmoOp = GizmoState["op"];
type GizmoSpace = GizmoState["space"];

export function Topbar() {
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const gizmo = useEditorStore((s) => s.gizmo);
  const setGizmo = useEditorStore((s) => s.setGizmo);
  const project = useEditorStore((s) => s.project);

  const ready = phase === "ready";

  const selectOp = (op: GizmoOp): void => {
    setGizmo({ op });
    void client.setGizmo({ op }).catch(() => {});
  };
  const selectSpace = (space: GizmoSpace): void => {
    setGizmo({ space });
    void client.setGizmo({ space }).catch(() => {});
  };

  return (
    <header className="flex h-[38px] flex-none items-center justify-between border-b border-border bg-card px-3">
      <div className="flex min-w-0 items-baseline gap-2">
        <span className="font-semibold tracking-tight">Saffron Editor</span>
        {project ? (
          <span className="truncate text-xs text-muted-foreground">{project.displayName}</span>
        ) : null}
      </div>
      <div className="flex items-center gap-2.5">
        <div
          className="flex items-center gap-0.5 rounded-md border border-border bg-background p-0.5"
          role="group"
          aria-label="Gizmo operation"
        >
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant={gizmo.op === "translate" ? "default" : "ghost"}
                onClick={() => selectOp("translate")}
                disabled={!ready}
                aria-pressed={gizmo.op === "translate"}
              >
                <Move3D />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Translate (W)</TooltipContent>
          </Tooltip>
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant={gizmo.op === "rotate" ? "default" : "ghost"}
                onClick={() => selectOp("rotate")}
                disabled={!ready}
                aria-pressed={gizmo.op === "rotate"}
              >
                <Rotate3D />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Rotate (E)</TooltipContent>
          </Tooltip>
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant={gizmo.op === "scale" ? "default" : "ghost"}
                onClick={() => selectOp("scale")}
                disabled={!ready}
                aria-pressed={gizmo.op === "scale"}
              >
                <Scaling />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Scale (R)</TooltipContent>
          </Tooltip>
        </div>
        <div
          className="flex items-center gap-0.5 rounded-md border border-border bg-background p-0.5"
          role="group"
          aria-label="Gizmo space"
        >
          <Button
            type="button"
            size="xs"
            variant={gizmo.space === "world" ? "default" : "ghost"}
            onClick={() => selectSpace("world")}
            disabled={!ready}
            title="World-space gizmo"
            aria-pressed={gizmo.space === "world"}
          >
            World
          </Button>
          <Button
            type="button"
            size="xs"
            variant={gizmo.space === "local" ? "default" : "ghost"}
            onClick={() => selectSpace("local")}
            disabled={!ready}
            title="Local-space gizmo"
            aria-pressed={gizmo.space === "local"}
          >
            Local
          </Button>
        </div>
      </div>
      <div className="text-[11px] uppercase tracking-wide text-muted-foreground">{phase}</div>
    </header>
  );
}
