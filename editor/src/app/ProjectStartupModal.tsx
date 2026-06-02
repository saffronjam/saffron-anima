import { useEffect, useMemo, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { FolderOpen, Plus, RefreshCcw } from "lucide-react";
import { client, type AppDataInfo, type ProjectInfo, type RecentProject } from "../control/client";
import { useEditorStore } from "../state/store";
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
import { Label } from "@/components/ui/label";

const PROJECT_JSON_FILTER = [{ name: "Saffron Project", extensions: ["json"] }];

interface ProjectStartupModalProps {
  open: boolean;
  onProjectLoaded(project: ProjectInfo): void;
}

export function ProjectStartupModal({
  open: modalOpen,
  onProjectLoaded,
}: ProjectStartupModalProps) {
  const resetSceneState = useEditorStore((s) => s.resetSceneState);
  const setProject = useEditorStore((s) => s.setProject);
  const setViewportHidden = useEditorStore((s) => s.setViewportHidden);
  const [info, setInfo] = useState<AppDataInfo | null>(null);
  const [recents, setRecents] = useState<RecentProject[]>([]);
  const [name, setName] = useState("");
  const [displayName, setDisplayName] = useState("");
  const [status, setStatus] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    setViewportHidden(modalOpen);
    return () => setViewportHidden(false);
  }, [modalOpen, setViewportHidden]);

  const nameError = useMemo(() => {
    if (name.length === 0) {
      return null;
    }
    return validProjectName(name)
      ? null
      : "Use lowercase letters, digits, and hyphens; start and end with a letter or digit.";
  }, [name]);

  const refreshRecents = async (): Promise<void> => {
    try {
      const [nextInfo, nextRecents] = await Promise.all([
        client.appDataInfo(),
        client.listRecentProjects(),
      ]);
      setInfo(nextInfo);
      setRecents(nextRecents.projects);
    } catch (err) {
      setStatus(errorText(err));
    }
  };

  useEffect(() => {
    if (modalOpen) {
      void refreshRecents();
    }
  }, [modalOpen]);

  const complete = async (project: ProjectInfo): Promise<void> => {
    setProject(project);
    resetSceneState();
    onProjectLoaded(project);
    const next = await client.rememberRecentProject({
      path: project.path,
      name: project.name,
      displayName: project.displayName,
      lastOpenedAt: new Date().toISOString(),
    });
    setRecents(next.projects);
  };

  const createProject = async (): Promise<void> => {
    if (!validProjectName(name)) {
      setStatus("Enter a valid project name.");
      return;
    }
    setBusy(true);
    setStatus(null);
    try {
      const project = await client.newProject(name, displayName.trim());
      await complete(project);
    } catch (err) {
      setStatus(errorText(err));
    } finally {
      setBusy(false);
    }
  };

  const openProjectPath = async (path: string): Promise<void> => {
    setBusy(true);
    setStatus(null);
    try {
      const project = await client.openProject(path);
      await complete(project);
    } catch (err) {
      setStatus(errorText(err));
    } finally {
      setBusy(false);
    }
  };

  const openProjectDirectory = async (): Promise<void> => {
    const selection = await open({ directory: true, multiple: false });
    if (typeof selection === "string") {
      await openProjectPath(selection);
    }
  };

  const openProjectFile = async (): Promise<void> => {
    const selection = await open({ multiple: false, filters: PROJECT_JSON_FILTER });
    if (typeof selection === "string") {
      await openProjectPath(selection);
    }
  };

  return (
    <Dialog open={modalOpen} onOpenChange={() => {}}>
      <DialogContent showCloseButton={false} className="sm:max-w-[680px]">
        <DialogHeader>
          <DialogTitle>Open a project</DialogTitle>
          <DialogDescription>
            Choose a recent project or create a new one before editing.
          </DialogDescription>
        </DialogHeader>

        <div className="grid min-h-[320px] gap-4 md:grid-cols-[1.1fr_0.9fr]">
          <section className="min-h-0 rounded-md border border-border bg-card">
            <div className="flex h-9 items-center justify-between border-b border-border px-3">
              <span className="text-xs font-medium uppercase text-muted-foreground">Recent</span>
              <Button
                type="button"
                size="icon-xs"
                variant="ghost"
                onClick={() => void refreshRecents()}
                disabled={busy}
                title="Refresh recent projects"
              >
                <RefreshCcw />
              </Button>
            </div>
            <div className="max-h-[280px] overflow-auto p-2">
              {recents.length === 0 ? (
                <div className="flex h-28 items-center justify-center text-sm text-muted-foreground">
                  No recent projects
                </div>
              ) : (
                <div className="space-y-1">
                  {recents.map((project) => (
                    <button
                      key={project.path}
                      type="button"
                      className="flex w-full flex-col gap-1 rounded-md px-3 py-2 text-left hover:bg-accent disabled:opacity-50"
                      disabled={busy}
                      onClick={() => void openProjectPath(project.path)}
                    >
                      <span className="truncate text-sm font-medium">{project.displayName}</span>
                      <span className="truncate font-mono text-[11px] text-muted-foreground">
                        {project.path}
                      </span>
                    </button>
                  ))}
                </div>
              )}
            </div>
          </section>

          <section className="space-y-4">
            <div className="space-y-2">
              <Label htmlFor="project-name">Project name</Label>
              <Input
                id="project-name"
                value={name}
                onChange={(event) => setName(event.target.value)}
                placeholder="a-name-like-this"
                aria-invalid={nameError !== null}
                disabled={busy}
              />
              {nameError ? <p className="text-xs text-destructive">{nameError}</p> : null}
            </div>
            <div className="space-y-2">
              <Label htmlFor="project-display-name">Display name</Label>
              <Input
                id="project-display-name"
                value={displayName}
                onChange={(event) => setDisplayName(event.target.value)}
                placeholder="A Name Like This"
                disabled={busy}
              />
            </div>
            <Button
              type="button"
              className="w-full"
              onClick={() => void createProject()}
              disabled={busy || !validProjectName(name)}
            >
              <Plus />
              Create Project
            </Button>

            <div className="grid gap-2 pt-2">
              <Button
                type="button"
                variant="outline"
                onClick={() => void openProjectDirectory()}
                disabled={busy}
              >
                <FolderOpen />
                Open Folder
              </Button>
              <Button
                type="button"
                variant="outline"
                onClick={() => void openProjectFile()}
                disabled={busy}
              >
                <FolderOpen />
                Open project.json
              </Button>
            </div>
          </section>
        </div>

        <DialogFooter className="items-center justify-between gap-3 sm:justify-between">
          <span className="min-w-0 truncate font-mono text-[11px] text-muted-foreground">
            {info ? info.userdataDir : ""}
          </span>
          {status ? (
            <span className="max-w-[320px] truncate text-xs text-destructive" title={status}>
              {status}
            </span>
          ) : null}
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

function validProjectName(name: string): boolean {
  if (name.length < 1 || name.length > 63) {
    return false;
  }
  return /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/.test(name);
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
