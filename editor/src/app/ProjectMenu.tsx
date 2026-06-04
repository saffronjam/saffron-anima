/// Project-level file operations exposed from the topbar project selector.
import { open, save } from "@tauri-apps/plugin-dialog";
import { ChevronDown } from "lucide-react";
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

const JSON_FILTER = [{ name: "Saffron Project / Scene", extensions: ["json"] }];
const MODEL_FILTER = [{ name: "Models", extensions: ["gltf", "glb", "obj", "smesh"] }];
const TEXTURE_FILTER = [
  { name: "Images", extensions: ["png", "jpg", "jpeg", "hdr", "tga", "bmp"] },
];
const PNG_FILTER = [{ name: "PNG image", extensions: ["png"] }];

export function ProjectMenu() {
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const refreshAssets = useEditorStore((s) => s.refreshAssets);
  const resetSceneState = useEditorStore((s) => s.resetSceneState);
  const setProject = useEditorStore((s) => s.setProject);
  const project = useEditorStore((s) => s.project);
  const nativeDialogOpen = useEditorStore((s) => s.nativeDialogOpen);

  const ready = phase === "ready";
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

  const loadProject = async (): Promise<void> => {
    const selection = await withNativeDialog(() => open({ multiple: false, filters: JSON_FILTER }));
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

  const openProjectFolder = async (): Promise<void> => {
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

  const saveScene = async (): Promise<void> => {
    const path = await withNativeDialog(() =>
      save({ defaultPath: "scene.json", filters: JSON_FILTER }),
    );
    if (!path) {
      return;
    }
    try {
      const res = await client.saveScene(path);
      notify(`Saved scene: ${res.path}`);
    } catch (err) {
      notify(`Save scene failed: ${errorText(err)}`);
    }
  };

  const loadScene = async (): Promise<void> => {
    const selection = await withNativeDialog(() => open({ multiple: false, filters: JSON_FILTER }));
    if (typeof selection !== "string") {
      return;
    }
    try {
      const res = await client.loadScene(selection);
      resetSceneState();
      notify(`Loaded scene: ${res.path}`);
    } catch (err) {
      notify(`Load scene failed: ${errorText(err)}`);
    }
  };

  const importModel = async (): Promise<void> => {
    const selection = await withNativeDialog(() =>
      open({ multiple: false, filters: MODEL_FILTER }),
    );
    if (typeof selection !== "string") {
      return;
    }
    try {
      await client.importModel(selection);
      await refreshAssets();
      notify("Imported model");
    } catch (err) {
      notify(`Import model failed: ${errorText(err)}`);
    }
  };

  const importTexture = async (): Promise<void> => {
    const selection = await withNativeDialog(() =>
      open({ multiple: false, filters: TEXTURE_FILTER }),
    );
    if (typeof selection !== "string") {
      return;
    }
    try {
      await client.importTexture(selection);
      await refreshAssets();
      notify("Imported texture");
    } catch (err) {
      notify(`Import texture failed: ${errorText(err)}`);
    }
  };

  const screenshotViewport = async (): Promise<void> => {
    const path = await withNativeDialog(() =>
      save({ defaultPath: "viewport.png", filters: PNG_FILTER }),
    );
    if (!path) {
      return;
    }
    try {
      const res = await client.screenshot("viewport", path);
      notify(res.pending ? `Screenshot queued: ${res.path}` : `Saved screenshot: ${res.path}`);
    } catch (err) {
      notify(`Screenshot failed: ${errorText(err)}`);
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
            title={label}
          >
            <span className="truncate">{label}</span>
            <ChevronDown />
          </Button>
        </DropdownMenuTrigger>
        <DropdownMenuContent align="start" className="min-w-52">
          <DropdownMenuItem onSelect={() => void saveProject()}>Save Project</DropdownMenuItem>
          <DropdownMenuItem onSelect={() => void saveProjectAs()}>
            Save Project As...
          </DropdownMenuItem>
          <DropdownMenuItem onSelect={() => void loadProject()}>Open Project...</DropdownMenuItem>
          <DropdownMenuItem onSelect={() => void openProjectFolder()}>
            Open Project Folder...
          </DropdownMenuItem>
          <DropdownMenuSeparator />
          <DropdownMenuItem onSelect={() => void saveScene()}>Save Scene...</DropdownMenuItem>
          <DropdownMenuItem onSelect={() => void loadScene()}>Load Scene...</DropdownMenuItem>
          <DropdownMenuSeparator />
          <DropdownMenuItem onSelect={() => void importModel()}>Import Model...</DropdownMenuItem>
          <DropdownMenuItem onSelect={() => void importTexture()}>
            Import Texture...
          </DropdownMenuItem>
          <DropdownMenuSeparator />
          <DropdownMenuItem onSelect={() => void screenshotViewport()}>
            Screenshot Viewport...
          </DropdownMenuItem>
        </DropdownMenuContent>
      </DropdownMenu>
    </div>
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
