/// Project-level file operations exposed from the topbar project selector.
import { invoke } from "@tauri-apps/api/core";
import { open, save } from "@tauri-apps/plugin-dialog";
import { ChevronDown, X } from "lucide-react";
import { client, type ProjectInfo } from "../control/client";
import { useEditorStore, withNativeDialog } from "../state/store";
import { notify } from "../lib/flash";
import { Button } from "@/components/ui/button";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";

const JSON_FILTER = [{ name: "Saffron Project", extensions: ["json"] }];

export function ProjectMenu() {
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const resetSceneState = useEditorStore((s) => s.resetSceneState);
  const setProject = useEditorStore((s) => s.setProject);
  const project = useEditorStore((s) => s.project);
  const nativeDialogOpen = useEditorStore((s) => s.nativeDialogOpen);
  const devMode = useEditorStore((s) => s.devMode);
  const setDevMode = useEditorStore((s) => s.setDevMode);
  const playState = useEditorStore((s) => s.playState);

  const ready = phase === "ready";
  // Saving/loading/reloading are locked during play: open/reload swap the scene out
  // from under the play duplicate (the engine rejects them with "stop play first"), and
  // save is greyed too so play-mode tweaks are never mistaken for authored, saved state.
  const editing = playState === "edit";
  const label = project?.displayName ?? "No project";

  const saveProject = async (): Promise<void> => {
    try {
      const res = await client.saveProject();
      setProject(res);
      await rememberProject(res);
      notify(`Saved project: ${res.path}`);
    } catch (err) {
      notify(`Save project failed: ${errorText(err)}`);
    }
  };

  const saveProjectAs = async (): Promise<void> => {
    const path = await withNativeDialog(() =>
      save({ defaultPath: project?.path ?? "project.json", filters: JSON_FILTER }),
    );
    if (!path) {
      return;
    }
    try {
      const res = await client.saveProject(path);
      setProject(res);
      await rememberProject(res);
      notify(`Saved project: ${res.path}`);
    } catch (err) {
      notify(`Save project failed: ${errorText(err)}`);
    }
  };

  const openProject = async (): Promise<void> => {
    const selection = await withNativeDialog(() => open({ directory: true, multiple: false }));
    if (typeof selection !== "string") {
      return;
    }
    try {
      const res = await client.openProject(selection);
      setProject(res);
      resetSceneState();
      await rememberProject(res);
      notify(`Loaded project: ${res.path}`);
    } catch (err) {
      notify(`Load project failed: ${errorText(err)}`);
    }
  };

  // Opens the project root (project.json, src/ scripts, assets/) in VS Code via the
  // dedicated Rust command — `code` may live on the host, not in the toolbox.
  const openInVsCode = async (): Promise<void> => {
    if (!project) {
      return;
    }
    try {
      await invoke("open_in_vscode", { path: project.root });
    } catch (err) {
      notify(`Open in VS Code failed: ${errorText(err)}`);
    }
  };

  const reloadProject = async (): Promise<void> => {
    try {
      const res = await client.reloadProject();
      setProject(res);
      resetSceneState();
      notify(`Reloaded project: ${res.path}`);
    } catch (err) {
      notify(`Reload project failed: ${errorText(err)}`);
    }
  };

  return (
    <div className="flex min-w-0 items-center gap-2">
      <DropdownMenu>
        <DropdownMenuTrigger asChild>
          <Button
            type="button"
            size="xs"
            variant="ghost"
            disabled={!ready || nativeDialogOpen}
            className="max-w-48 justify-start px-1.5 text-muted-foreground"
          >
            <span className="truncate">{label}</span>
            <ChevronDown />
          </Button>
        </DropdownMenuTrigger>
        <DropdownMenuContent align="start" className="min-w-52">
          <DropdownMenuItem onSelect={() => void saveProject()} disabled={!editing}>
            Save Project
          </DropdownMenuItem>
          <DropdownMenuItem onSelect={() => void saveProjectAs()} disabled={!editing}>
            Save Project As...
          </DropdownMenuItem>
          <DropdownMenuItem onSelect={() => void openProject()} disabled={!editing}>
            Open Project...
          </DropdownMenuItem>
          <DropdownMenuItem onSelect={() => void openInVsCode()} disabled={!project}>
            Open in VS Code
          </DropdownMenuItem>
          {devMode ? (
            <>
              <DropdownMenuSeparator />
              <DropdownMenuItem
                onSelect={() => void reloadProject()}
                disabled={!project || !editing}
              >
                Reload Project
              </DropdownMenuItem>
            </>
          ) : null}
        </DropdownMenuContent>
      </DropdownMenu>
      {devMode ? <DevModeChip onExit={() => setDevMode(false)} /> : null}
    </div>
  );
}

/// Status chip flagging that developer mode is on; the X exits dev mode.
function DevModeChip({ onExit }: { onExit: () => void }) {
  return (
    <span className="flex h-5 flex-none select-none items-center gap-1 rounded-full bg-orange-500/15 py-0 pl-2 pr-1 text-[10px] font-medium uppercase tracking-wide text-orange-400">
      Dev mode
      <button
        type="button"
        aria-label="Exit developer mode"
        className="flex size-3.5 items-center justify-center rounded-full hover:bg-orange-500/25"
        onClick={onExit}
      >
        <X className="size-3" />
      </button>
    </span>
  );
}

function errorText(err: unknown): string {
  if (typeof err === "string") {
    return err;
  }
  if (err instanceof Error) {
    return err.message;
  }
  return String(err);
}

async function rememberProject(project: ProjectInfo): Promise<void> {
  await client.rememberRecentProject({
    path: project.path,
    name: project.name,
    displayName: project.displayName,
    lastOpenedAt: new Date().toISOString(),
  });
}
