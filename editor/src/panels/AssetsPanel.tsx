/// The Assets panel: the React port of the C++ `assetCatalogPanel`
/// (editor_panels.cpp:226-325). A responsive tile grid over `store.assets`, an
/// Import button (Tauri file dialog), an OS file-drop target, and the View modal.
/// Imports route by extension (parity with `importToCatalog`,
/// editor_app.cppm:188-205): images → import-texture (no spawn), everything else →
/// import-model (spawns + selects an entity, matching `se import-model`).
import { useCallback, useEffect, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import { Plus } from "lucide-react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import { AssetTile } from "../components/AssetTile";
import { AssetViewer } from "../components/AssetViewer";
import type { AssetEntry } from "../protocol";
import { Button } from "@/components/ui/button";
import { ScrollArea } from "@/components/ui/scroll-area";

/// Image extensions that import as a catalog texture; everything else is imported
/// as a model (parity with editor_app.cppm:201).
const TEXTURE_EXTS = new Set(["png", "jpg", "jpeg", "hdr", "tga", "bmp"]);

/// Model + image extensions offered in the file dialog.
const MODEL_EXTS = ["gltf", "glb", "obj", "smesh"];
const IMAGE_EXTS = ["png", "jpg", "jpeg", "hdr", "tga", "bmp"];

function extensionOf(path: string): string {
  const dot = path.lastIndexOf(".");
  return dot >= 0 ? path.slice(dot + 1).toLowerCase() : "";
}

/// Import one filesystem path through the right command, then refresh the catalog.
async function importPath(path: string, refreshAssets: () => Promise<void>): Promise<void> {
  try {
    if (TEXTURE_EXTS.has(extensionOf(path))) {
      await client.importTexture(path);
    } else {
      await client.importModel(path);
    }
  } catch {
    // The engine reports a bad import as ok:false (a rejected promise); swallow so
    // a single bad file doesn't break a multi-file drop.
  }
  await refreshAssets();
}

export function AssetsPanel() {
  const assets = useEditorStore((s) => s.assets);
  const refreshAssets = useEditorStore((s) => s.refreshAssets);
  const [viewing, setViewing] = useState<AssetEntry | null>(null);
  const [dropActive, setDropActive] = useState(false);

  const importMany = useCallback(
    async (paths: string[]): Promise<void> => {
      for (const path of paths) {
        await importPath(path, refreshAssets);
      }
    },
    [refreshAssets],
  );

  // OS file-drop: Tauri delivers native file drops via the webview drag-drop event
  // (a distinct channel from the HTML5 `application/x-se-asset` tile DnD). We only
  // import when the drop position is inside this panel's rect, so dropping a model
  // on the viewport doesn't trigger a catalog import here.
  useEffect(() => {
    let unlisten: (() => void) | null = null;
    let disposed = false;
    void getCurrentWebview()
      .onDragDropEvent((event) => {
        const payload = event.payload;
        if (payload.type === "enter" || payload.type === "over") {
          setDropActive(isInsidePanel(payload.position));
        } else if (payload.type === "leave") {
          setDropActive(false);
        } else if (payload.type === "drop") {
          setDropActive(false);
          if (payload.paths.length > 0 && isInsidePanel(payload.position)) {
            void importMany(payload.paths);
          }
        }
      })
      .then((fn) => {
        if (disposed) {
          fn();
        } else {
          unlisten = fn;
        }
      });
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [importMany]);

  const onImportClick = useCallback(async (): Promise<void> => {
    const selection = await open({
      multiple: true,
      filters: [
        { name: "Models & Images", extensions: [...MODEL_EXTS, ...IMAGE_EXTS] },
        { name: "Models", extensions: MODEL_EXTS },
        { name: "Images", extensions: IMAGE_EXTS },
      ],
    });
    if (!selection) {
      return;
    }
    await importMany(Array.isArray(selection) ? selection : [selection]);
  }, [importMany]);

  return (
    <div
      className="flex h-full min-h-0 flex-col"
      data-asset-panel="true"
    >
      <div className="flex h-10 flex-none items-center justify-between border-b border-border px-3">
        <span className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">
          Assets
        </span>
        <Button
          type="button"
          size="sm"
          variant="ghost"
          className="gap-1 text-muted-foreground hover:text-foreground"
          onClick={() => void onImportClick()}
          title="Import a model or texture"
        >
          <Plus />
          Import
        </Button>
      </div>
      <ScrollArea className="min-h-0 flex-1">
        <div
          className={
            dropActive
              ? "min-h-full rounded-sm p-2 ring-2 ring-inset ring-ring"
              : "min-h-full p-2"
          }
        >
          {assets.length === 0 ? (
            <p className="px-1 py-3 text-center text-xs italic text-muted-foreground">
              No assets yet — import or drag-and-drop a model or texture.
            </p>
          ) : (
            <div
              className="grid gap-2"
              style={{ gridTemplateColumns: "repeat(auto-fill, minmax(72px, 1fr))" }}
            >
              {assets.map((asset) => (
                <AssetTile key={asset.id} entry={asset} onView={setViewing} />
              ))}
            </div>
          )}
        </div>
      </ScrollArea>
      <AssetViewer entry={viewing} onClose={() => setViewing(null)} />
    </div>
  );
}

/// Hit-test a physical-pixel drop position against the Assets panel's DOM rect.
/// (The drop event reports physical pixels; getBoundingClientRect is CSS pixels, so
/// we scale by devicePixelRatio.)
function isInsidePanel(position: { x: number; y: number }): boolean {
  const el = document.querySelector('[data-asset-panel="true"]');
  if (!el) {
    return false;
  }
  const rect = el.getBoundingClientRect();
  const scale = window.devicePixelRatio || 1;
  const cssX = position.x / scale;
  const cssY = position.y / scale;
  return (
    cssX >= rect.left && cssX <= rect.right && cssY >= rect.top && cssY <= rect.bottom
  );
}
