/// The topbar gizmo group: T/R/S operation buttons, a world/local space toggle, and a
/// preserve-children toggle (transform a parent without moving its children), wired to
/// the engine's single gizmo state (`set-gizmo`/`get-gizmo`). Clicks set `store.gizmo`
/// optimistically and fire `set-gizmo`; the reconcile poll's `get-gizmo` read keeps it
/// in sync with external mutations (e.g. `sa set-gizmo`).
import { Fragment } from "react";
import {
  Anchor,
  Move3D,
  Pause,
  Play,
  Redo2,
  Rotate3D,
  Scaling,
  Settings,
  Square,
  StepForward,
  Undo2,
  Wrench,
} from "lucide-react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { canRedo, canUndo, redoLabel, undoLabel } from "../lib/undo";
import { useShallow } from "zustand/react/shallow";
import { COMMANDS_BY_ID, bindingFor, formatBinding, type CommandId } from "../lib/keybindings";
import { notify } from "../lib/flash";
import type { GizmoState } from "../protocol";
import { Button } from "@/components/ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import { ProjectMenu } from "../app/ProjectMenu";
import { AlarmBadge } from "../components/AlarmBadge";
import { SCENE_PANEL_REGISTRY, type PanelGroup } from "@/components/dock/panelRegistry";
import { logRender } from "../lib/renderLog";

/// The closable Scene panels, in registry order — the Tools-menu reopen list.
const SCENE_PANEL_MENU = Object.values(SCENE_PANEL_REGISTRY).filter((def) => def.closable);

/// Tools-menu groups, in display order.
const TOOL_GROUPS: { label: string; group: PanelGroup }[] = [
  { label: "Editing", group: "editing" },
  { label: "Diagnostics", group: "diagnostics" },
];

type GizmoOp = GizmoState["op"];
type GizmoSpace = GizmoState["space"];

export function Topbar() {
  logRender("Topbar");
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const gizmo = useEditorStore((s) => s.gizmo);
  const setGizmo = useEditorStore((s) => s.setGizmo);
  const playState = useEditorStore((s) => s.playState);
  const setPlayState = useEditorStore((s) => s.setPlayState);
  const keyBindings = useEditorStore((s) => s.keyBindings);
  const openPanel = useEditorStore((s) => s.openPanel);
  const setSettingsOpen = useEditorStore((s) => s.setSettingsOpen);
  const undo = useEditorStore((s) => s.undo);
  const redo = useEditorStore((s) => s.redo);
  // The active main tab's history drives the undo/redo buttons' enabled state + labels.
  const history = useEditorStore(
    useShallow((s) => {
      const h = s.historyByTab[s.activeViewTabId];
      // The scene tab targets the throwaway play duplicate while playing, so its undo is
      // paused until Stop (the pre-play history is preserved, not cleared).
      const locked = s.activeViewTabId === "scene" && s.playState !== "edit";
      return {
        canUndo: !locked && !!h && canUndo(h),
        canRedo: !locked && !!h && canRedo(h),
        undoLabel: h ? undoLabel(h) : null,
        redoLabel: h ? redoLabel(h) : null,
      };
    }),
  );

  const ready = phase === "ready";
  // The gizmo is hidden during play (the engine rejects gizmo commands), so its
  // controls grey out; playback rides `ready` alone.
  const editing = ready && playState === "edit";

  // The configured shortcut for a gizmo command, formatted for a tooltip suffix.
  const shortcut = (id: CommandId): string =>
    formatBinding(COMMANDS_BY_ID[id], bindingFor(id, keyBindings));

  const selectOp = (op: GizmoOp): void => {
    setGizmo({ op });
    void client.setGizmo({ op }).catch(() => {});
  };
  const selectSpace = (space: GizmoSpace): void => {
    setGizmo({ space });
    void client.setGizmo({ space }).catch(() => {});
  };
  const togglePreserveChildren = (): void => {
    const preserveChildren = !gizmo.preserveChildren;
    setGizmo({ preserveChildren });
    void client.setGizmo({ preserveChildren }).catch(() => {});
  };

  // Playback: optimistic store write + fire the command; the reconcile poll repairs
  // the state on failure (and reflects an external `sa play`). The viewport cuts to
  // the scene camera and the chrome tints while not in edit (see Layout).
  const onPlayPause = (): void => {
    if (playState === "playing") {
      setPlayState("paused");
      void client.pause().catch(() => setPlayState("playing"));
      return;
    }
    const previous = playState; // edit or paused
    setPlayState("playing");
    void client
      .play()
      .then((result) => {
        if (!result.hasPrimaryCamera) {
          notify("No primary camera — using the editor camera");
        }
      })
      .catch(() => setPlayState(previous));
  };
  const onStop = (): void => {
    const previous = playState;
    setPlayState("edit");
    void client.stop().catch(() => setPlayState(previous));
  };
  const onStep = (): void => {
    void client.step().catch(() => {});
  };

  return (
    <header
      className={`flex h-12 flex-none items-center justify-between border-b bg-background px-4 ${
        playState === "edit" ? "border-border" : "border-amber-500/60 bg-amber-500/5"
      }`}
    >
      <div className="flex min-w-0 items-center gap-2">
        <ProjectMenu />
        <div className="flex items-center gap-0.5" role="group" aria-label="History">
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant="ghost"
                onClick={() => void undo()}
                disabled={!ready || !history.canUndo}
                aria-label="Undo"
              >
                <Undo2 />
              </Button>
            </TooltipTrigger>
            <TooltipContent>
              {history.undoLabel ? `Undo ${history.undoLabel}` : "Undo"} ({shortcut("edit.undo")})
            </TooltipContent>
          </Tooltip>
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant="ghost"
                onClick={() => void redo()}
                disabled={!ready || !history.canRedo}
                aria-label="Redo"
              >
                <Redo2 />
              </Button>
            </TooltipTrigger>
            <TooltipContent>
              {history.redoLabel ? `Redo ${history.redoLabel}` : "Redo"} ({shortcut("edit.redo")})
            </TooltipContent>
          </Tooltip>
        </div>
      </div>
      <div className="flex items-center gap-2.5">
        <div
          className="flex items-center gap-0.5 rounded-md border border-border bg-background p-0.5"
          role="group"
          aria-label="Playback"
        >
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant={playState === "edit" ? "ghost" : "default"}
                onClick={onPlayPause}
                disabled={!ready}
                aria-pressed={playState !== "edit"}
                aria-label={playState === "playing" ? "Pause" : "Play"}
              >
                {playState === "playing" ? <Pause /> : <Play />}
              </Button>
            </TooltipTrigger>
            <TooltipContent>
              {playState === "playing"
                ? "Pause (Ctrl+Shift+P)"
                : playState === "paused"
                  ? "Resume (Ctrl+Shift+P)"
                  : "Play (Ctrl+P)"}
            </TooltipContent>
          </Tooltip>
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant="ghost"
                onClick={onStop}
                disabled={!ready || playState === "edit"}
                aria-label="Stop"
              >
                <Square />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Stop (Ctrl+P)</TooltipContent>
          </Tooltip>
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant="ghost"
                onClick={onStep}
                disabled={!ready || playState !== "paused"}
                aria-label="Step"
              >
                <StepForward />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Step one frame (Ctrl+Alt+P)</TooltipContent>
          </Tooltip>
        </div>
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
                disabled={!editing}
                aria-pressed={gizmo.op === "translate"}
              >
                <Move3D />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Translate ({shortcut("gizmo.translate")})</TooltipContent>
          </Tooltip>
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant={gizmo.op === "rotate" ? "default" : "ghost"}
                onClick={() => selectOp("rotate")}
                disabled={!editing}
                aria-pressed={gizmo.op === "rotate"}
              >
                <Rotate3D />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Rotate ({shortcut("gizmo.rotate")})</TooltipContent>
          </Tooltip>
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant={gizmo.op === "scale" ? "default" : "ghost"}
                onClick={() => selectOp("scale")}
                disabled={!editing}
                aria-pressed={gizmo.op === "scale"}
              >
                <Scaling />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Scale ({shortcut("gizmo.scale")})</TooltipContent>
          </Tooltip>
        </div>
        <div
          className="flex items-center gap-0.5 rounded-md border border-border bg-background p-0.5"
          role="group"
          aria-label="Gizmo space"
        >
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="xs"
                variant={gizmo.space === "world" ? "default" : "ghost"}
                onClick={() => selectSpace("world")}
                disabled={!editing}
                aria-pressed={gizmo.space === "world"}
              >
                World
              </Button>
            </TooltipTrigger>
            <TooltipContent>World-space gizmo</TooltipContent>
          </Tooltip>
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="xs"
                variant={gizmo.space === "local" ? "default" : "ghost"}
                onClick={() => selectSpace("local")}
                disabled={!editing}
                aria-pressed={gizmo.space === "local"}
              >
                Local
              </Button>
            </TooltipTrigger>
            <TooltipContent>Local-space gizmo</TooltipContent>
          </Tooltip>
        </div>
        <div
          className="flex items-center gap-0.5 rounded-md border border-border bg-background p-0.5"
          role="group"
          aria-label="Transform options"
        >
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-xs"
                variant={gizmo.preserveChildren ? "default" : "ghost"}
                onClick={togglePreserveChildren}
                disabled={!editing}
                aria-pressed={gizmo.preserveChildren}
              >
                <Anchor />
              </Button>
            </TooltipTrigger>
            <TooltipContent>
              Preserve children — transform a parent without moving its children
            </TooltipContent>
          </Tooltip>
        </div>
      </div>
      <div className="flex items-center justify-end gap-1.5">
        <AlarmBadge />
        <div className="flex items-center gap-0.5" role="group" aria-label="Tools">
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button type="button" size="icon-sm" variant="ghost" aria-label="Tools">
                <Wrench />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end" className="min-w-40">
              {TOOL_GROUPS.filter((g) => SCENE_PANEL_MENU.some((def) => def.group === g.group)).map(
                (g, gi) => (
                  <Fragment key={g.group}>
                    {gi > 0 ? <DropdownMenuSeparator /> : null}
                    <DropdownMenuLabel>{g.label}</DropdownMenuLabel>
                    {SCENE_PANEL_MENU.filter((def) => def.group === g.group).map((def) => (
                      <DropdownMenuItem key={def.id} onSelect={() => openPanel(def.id)}>
                        {def.title}
                      </DropdownMenuItem>
                    ))}
                  </Fragment>
                ),
              )}
            </DropdownMenuContent>
          </DropdownMenu>
          <Button
            type="button"
            size="icon-sm"
            variant="ghost"
            onClick={() => setSettingsOpen(true)}
            aria-label="Editor settings"
          >
            <Settings />
          </Button>
        </div>
      </div>
    </header>
  );
}
