/// The assets-panel detail view: a slide-in overlay on the right edge of the grid
/// that shows on-disk metadata for the single selected asset (size, vertex/triangle
/// counts for meshes, modified time), fetched via `probe-asset`.
import { X } from "lucide-react";
import type { AssetMetadataDto } from "../protocol";
import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";

function formatBytes(bytes: number): string {
  if (bytes < 1024) {
    return `${bytes} B`;
  }
  const units = ["KB", "MB", "GB", "TB"];
  let value = bytes / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(value < 10 ? 1 : 0)} ${units[unit]}`;
}

function formatDate(unixSeconds: number): string {
  if (!unixSeconds) {
    return "—";
  }
  return new Date(unixSeconds * 1000).toLocaleString();
}

function Row({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex flex-col gap-0.5">
      <span className="text-[10px] uppercase tracking-wide text-muted-foreground">{label}</span>
      <span className="break-words font-mono text-xs text-foreground">{value}</span>
    </div>
  );
}

export function AssetMetadataPanel({
  metadata,
  open,
  onClose,
}: {
  metadata: AssetMetadataDto | null;
  open: boolean;
  onClose(): void;
}) {
  return (
    <div
      className={cn(
        "absolute inset-y-0 right-0 z-20 w-64 border-l border-border bg-background shadow-lg transition-transform duration-200 ease-out",
        open ? "translate-x-0" : "translate-x-full",
      )}
      aria-hidden={!open}
    >
      <div className="flex h-8 flex-none items-center justify-between border-b border-border px-2">
        <span className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">
          Details
        </span>
        <Button type="button" size="icon-xs" variant="ghost" onClick={onClose} title="Close">
          <X />
        </Button>
      </div>
      {metadata ? (
        <div className="flex flex-col gap-2.5 p-3">
          <Row label="Filename" value={metadata.name} />
          <Row label="Location" value={metadata.folder ?? "Root"} />
          <Row label="Type" value={metadata.type} />
          <Row label="Size" value={formatBytes(metadata.sizeBytes)} />
          {metadata.vertexCount !== undefined ? (
            <Row label="Vertices" value={metadata.vertexCount.toLocaleString()} />
          ) : null}
          {metadata.triangleCount !== undefined ? (
            <Row label="Triangles" value={metadata.triangleCount.toLocaleString()} />
          ) : null}
          <Row label="Created" value={formatDate(metadata.createdAt)} />
        </div>
      ) : (
        <p className="p-3 text-xs italic text-muted-foreground">Loading…</p>
      )}
    </div>
  );
}
