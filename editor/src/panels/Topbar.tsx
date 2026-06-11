/// The topbar gizmo group: T/R/S operation buttons, a world/local space toggle, and a
/// preserve-children toggle (transform a parent without moving its children), wired to
/// the engine's single gizmo state (`set-gizmo`/`get-gizmo`). Clicks set `store.gizmo`
/// optimistically and fire `set-gizmo`; the reconcile poll's `get-gizmo` read keeps it
/// in sync with external mutations (e.g. `se set-gizmo`).
import {
  Anchor,
  Clapperboard,
  Move3D,
  Pause,
  Play,
  Rotate3D,
  Scaling,
  Settings,
  Square,
  StepForward,
  Wrench,
} from "lucide-react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { COMMANDS_BY_ID, bindingFor, formatBinding, type CommandId } from "../lib/keybindings";
import { notify } from "../lib/flash";
import type { GizmoState } from "../protocol";
import { Button } from "@/components/ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import { ProjectMenu } from "../app/ProjectMenu";
import { AlarmBadge } from "../components/AlarmBadge";
import { logRender } from "../lib/renderLog";

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
  const openRightTool = useEditorStore((s) => s.openRightTool);
  const openBottomTool = useEditorStore((s) => s.openBottomTool);
  const setSettingsOpen = useEditorStore((s) => s.setSettingsOpen);

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
  // the state on failure (and reflects an external `se play`). The viewport cuts to
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
      <div className="flex min-w-0 items-baseline gap-2">
        <ProjectMenu />
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
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant="ghost"
                onClick={() => openBottomTool("timeline")}
                aria-label="Timeline"
              >
                <Clapperboard />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Timeline</TooltipContent>
          </Tooltip>
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button type="button" size="icon-sm" variant="ghost" aria-label="Tools">
                <Wrench />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end" className="min-w-40">
              <DropdownMenuItem onSelect={() => openRightTool("stats")}>Stats</DropdownMenuItem>
              <DropdownMenuItem onSelect={() => openRightTool("profiler")}>
                Profiler
              </DropdownMenuItem>
              <DropdownMenuItem onSelect={() => openRightTool("material")}>Material</DropdownMenuItem>
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
