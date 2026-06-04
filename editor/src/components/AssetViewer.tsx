/// The asset View modal: a large (512 px) square preview of a single catalog asset
/// (parity with `viewerPanel`, editor_panels.cpp:327-351, and the 512 re-render at
/// editor_app.cppm:165). A shadcn Dialog, opened by a tile double-click. The native
/// viewport is a reparented X11 child that always paints over the webview, so a
/// dialog centered on the screen would be covered by it.
/// So while the viewer is open we set `store.viewportHidden`, which the
/// ViewportPanel reacts to by parking the native window off-screen. The preview is
/// a base64 PNG fetched over the socket and rendered in the webview.
import { useEffect, useState } from "react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import type { AssetEntry } from "../protocol";
import { Dialog, DialogContent, DialogHeader, DialogTitle } from "@/components/ui/dialog";
import { cn } from "@/lib/utils";

/// Preview render size (px), matching the C++ View re-render.
const VIEW_SIZE = 512;

function base64ToBlobUrl(b64: string): string {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) {
    bytes[i] = bin.charCodeAt(i);
  }
  return URL.createObjectURL(new Blob([bytes], { type: "image/png" }));
}

export interface AssetViewerProps {
  /// The asset to preview, or null when the modal is closed.
  entry: AssetEntry | null;
  onClose(): void;
}

export function AssetViewer({ entry, onClose }: AssetViewerProps) {
  const open = entry !== null;
  return (
    <Dialog
      open={open}
      onOpenChange={(next) => {
        if (!next) {
          onClose();
        }
      }}
    >
      <DialogContent className="sm:max-w-[560px]">
        <DialogHeader>
          <DialogTitle className="truncate font-mono text-sm">{entry?.name ?? "Asset"}</DialogTitle>
        </DialogHeader>
        {entry ? <AssetPreview entry={entry} /> : null}
      </DialogContent>
    </Dialog>
  );
}

export function AssetPreview({ entry, className }: { entry: AssetEntry; className?: string }) {
  const [url, setUrl] = useState<string | null>(null);
  const setViewportHidden = useEditorStore((s) => s.setViewportHidden);

  // Park the native viewport off-screen while a webview preview occupies the main
  // content area; restore it on unmount.
  useEffect(() => {
    setViewportHidden(true);
    return () => {
      setViewportHidden(false);
    };
  }, [setViewportHidden]);

  // Fetch the 512 preview when the asset changes; revoke the previous URL and the
  // final URL on close/unmount (no blob leak — this URL is NOT the shared cache).
  useEffect(() => {
    let cancelled = false;
    let created: string | null = null;
    void client
      .viewAsset(entry.id, VIEW_SIZE)
      .then((thumb) => {
        if (cancelled) {
          return;
        }
        created = base64ToBlobUrl(thumb.base64);
        setUrl(created);
      })
      .catch(() => {
        // Leave `url` null; the dialog shows the empty preview frame.
      });
    return () => {
      cancelled = true;
      if (created) {
        URL.revokeObjectURL(created);
      }
      setUrl(null);
    };
  }, [entry]);

  return (
    <div
      className={cn(
        "flex aspect-square w-full items-center justify-center overflow-hidden rounded-md border border-border bg-muted",
        className,
      )}
    >
      {url ? (
        <img src={url} alt={entry.name} className="size-full object-contain" draggable={false} />
      ) : (
        <span className="text-xs italic text-muted-foreground">Rendering preview...</span>
      )}
    </div>
  );
}
