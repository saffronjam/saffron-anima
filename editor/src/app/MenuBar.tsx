/// The top menu bar: File (save/load project + scene, import model/texture,
/// screenshot) and Create (the `add-entity` presets). It mirrors the C++ Create +
/// File menus. File paths come from the Tauri dialog plugin (`open`/`save`).
///
/// Load Project / Load Scene call `store.resetSceneState()` AFTER the engine
/// confirms the load: the engine clears its own selection on load, so the store
/// must mirror that (no stale entities/selection survive) and let the reconcile
/// poll re-fetch against the freshly loaded scene.
///
/// The bar lives ABOVE the embedded viewport, so its dropdowns drop down over the
/// non-viewport chrome (left sidebar / topbar area) and are never occluded by the
/// reparented native window — the same constraint that keeps the asset/inspector
/// popovers in the side docks.
import { useState } from "react";
import { open, save } from "@tauri-apps/plugin-dialog";
import { client, type EntityPreset, type ProjectInfo } from "../control/client";
import { useEditorStore } from "../state/store";
import { CREATE_PRESETS } from "./CreateMenu";
import {
  Menubar,
  MenubarContent,
  MenubarItem,
  MenubarMenu,
  MenubarSeparator,
  MenubarTrigger,
} from "@/components/ui/menubar";

const JSON_FILTER = [{ name: "Saffron Project / Scene", extensions: ["json"] }];
const MODEL_FILTER = [{ name: "Models", extensions: ["gltf", "glb", "obj", "smesh"] }];
const TEXTURE_FILTER = [
  { name: "Images", extensions: ["png", "jpg", "jpeg", "hdr", "tga", "bmp"] },
];
const PNG_FILTER = [{ name: "PNG image", extensions: ["png"] }];

export function MenuBar() {
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const selectEntity = useEditorStore((s) => s.selectEntity);
  const refreshAssets = useEditorStore((s) => s.refreshAssets);
  const resetSceneState = useEditorStore((s) => s.resetSceneState);
  const setProject = useEditorStore((s) => s.setProject);
  const project = useEditorStore((s) => s.project);
  const [status, setStatus] = useState<string | null>(null);

  const ready = phase === "ready";

  const flash = (message: string): void => {
    setStatus(message);
    window.setTimeout(() => {
      // Only clear if it is still our message (a newer flash wins).
      setStatus((current) => (current === message ? null : current));
    }, 4000);
  };

  const saveProject = async (): Promise<void> => {
    try {
      const res = await client.saveProject();
      setProject(res);
      await rememberProject(res);
      flash(`Saved project → ${res.path}`);
    } catch (err) {
      flash(`Save project failed: ${errorText(err)}`);
    }
  };

  const saveProjectAs = async (): Promise<void> => {
    const path = await save({ defaultPath: project?.path ?? "project.json", filters: JSON_FILTER });
    if (!path) {
      return;
    }
    try {
      const res = await client.saveProject(path);
      setProject(res);
      await rememberProject(res);
      flash(`Saved project → ${res.path}`);
    } catch (err) {
      flash(`Save project failed: ${errorText(err)}`);
    }
  };

  const loadProject = async (): Promise<void> => {
    const selection = await open({ multiple: false, filters: JSON_FILTER });
    if (typeof selection !== "string") {
      return;
    }
    try {
      const res = await client.openProject(selection);
      // Mirror the engine's own reset: clear entities/selection/assets/env and let
      // the poll re-fetch against the loaded scene.
      setProject(res);
      resetSceneState();
      await rememberProject(res);
      flash(`Loaded project ← ${res.path}`);
    } catch (err) {
      flash(`Load project failed: ${errorText(err)}`);
    }
  };

  const openProjectFolder = async (): Promise<void> => {
    const selection = await open({ directory: true, multiple: false });
    if (typeof selection !== "string") {
      return;
    }
    try {
      const res = await client.openProject(selection);
      setProject(res);
      resetSceneState();
      await rememberProject(res);
      flash(`Loaded project ← ${res.path}`);
    } catch (err) {
      flash(`Load project failed: ${errorText(err)}`);
    }
  };

  const saveScene = async (): Promise<void> => {
    const path = await save({ defaultPath: "scene.json", filters: JSON_FILTER });
    if (!path) {
      return;
    }
    try {
      const res = await client.saveScene(path);
      flash(`Saved scene → ${res.path}`);
    } catch (err) {
      flash(`Save scene failed: ${errorText(err)}`);
    }
  };

  const loadScene = async (): Promise<void> => {
    const selection = await open({ multiple: false, filters: JSON_FILTER });
    if (typeof selection !== "string") {
      return;
    }
    try {
      const res = await client.loadScene(selection);
      resetSceneState();
      flash(`Loaded scene ← ${res.path}`);
    } catch (err) {
      flash(`Load scene failed: ${errorText(err)}`);
    }
  };

  const importModel = async (): Promise<void> => {
    const selection = await open({ multiple: false, filters: MODEL_FILTER });
    if (typeof selection !== "string") {
      return;
    }
    try {
      await client.importModel(selection);
      // Catalog grew; refresh the asset list (no selection reset on import).
      await refreshAssets();
      flash("Imported model");
    } catch (err) {
      flash(`Import model failed: ${errorText(err)}`);
    }
  };

  const importTexture = async (): Promise<void> => {
    const selection = await open({ multiple: false, filters: TEXTURE_FILTER });
    if (typeof selection !== "string") {
      return;
    }
    try {
      await client.importTexture(selection);
      await refreshAssets();
      flash("Imported texture");
    } catch (err) {
      flash(`Import texture failed: ${errorText(err)}`);
    }
  };

  const screenshotViewport = async (): Promise<void> => {
    const path = await save({ defaultPath: "viewport.png", filters: PNG_FILTER });
    if (!path) {
      return;
    }
    try {
      // Viewport capture is synchronous (pending:false); the file exists on return.
      const res = await client.screenshot("viewport", path);
      flash(res.pending ? `Screenshot queued → ${res.path}` : `Saved screenshot → ${res.path}`);
    } catch (err) {
      flash(`Screenshot failed: ${errorText(err)}`);
    }
  };

  const create = (preset: EntityPreset): void => {
    void client
      .addEntity(preset)
      .then((ref) => selectEntity(ref.id))
      .catch((err: unknown) => flash(`Create failed: ${errorText(err)}`));
  };

  return (
    <div className="flex h-[34px] flex-none items-center gap-3 border-b border-border bg-card px-2">
      <Menubar className="h-7 border-0 bg-transparent p-0 shadow-none">
        <MenubarMenu>
          <MenubarTrigger disabled={!ready} className="h-7 px-2 text-[12px]">
            File
          </MenubarTrigger>
          <MenubarContent align="start" className="min-w-48">
            <MenubarItem onSelect={() => void saveProject()}>Save Project</MenubarItem>
            <MenubarItem onSelect={() => void saveProjectAs()}>Save Project As…</MenubarItem>
            <MenubarItem onSelect={() => void loadProject()}>Open Project…</MenubarItem>
            <MenubarItem onSelect={() => void openProjectFolder()}>
              Open Project Folder…
            </MenubarItem>
            <MenubarItem onSelect={() => void saveScene()}>Save Scene…</MenubarItem>
            <MenubarItem onSelect={() => void loadScene()}>Load Scene…</MenubarItem>
            <MenubarSeparator />
            <MenubarItem onSelect={() => void importModel()}>Import Model…</MenubarItem>
            <MenubarItem onSelect={() => void importTexture()}>Import Texture…</MenubarItem>
            <MenubarSeparator />
            <MenubarItem onSelect={() => void screenshotViewport()}>
              Screenshot Viewport…
            </MenubarItem>
          </MenubarContent>
        </MenubarMenu>
        <MenubarMenu>
          <MenubarTrigger disabled={!ready} className="h-7 px-2 text-[12px]">
            Create
          </MenubarTrigger>
          <MenubarContent align="start" className="min-w-44">
            {CREATE_PRESETS.map(({ label, preset, icon: Icon }) => (
              <MenubarItem key={preset} onSelect={() => create(preset)}>
                <Icon className="size-4 text-muted-foreground" />
                {label}
              </MenubarItem>
            ))}
          </MenubarContent>
        </MenubarMenu>
      </Menubar>
      {status ? (
        <span className="truncate text-[11px] text-muted-foreground" title={status}>
          {status}
        </span>
      ) : null}
    </div>
  );
}

/// Normalize a rejected control call (the Rust passthrough rejects with the
/// engine's error string) into a readable message.
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
