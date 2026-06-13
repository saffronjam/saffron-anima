/// The asset editor's right panel (shown only when the model has clips): the model's own clips (read
/// from get-asset-model, i.e. its .smodel container's animation sub-assets — not the whole catalog) and
/// a details section for the focused clip or the model. Clicking a clip loads it paused at frame 0 on
/// the previewed model (UE5's pick semantics: select loads, the transport plays). The active row follows
/// engine truth from the reconcile poll, with an optimistic highlight in between.
import { useEffect, useState } from "react";
import { ScrollArea } from "@/components/ui/scroll-area";
import { cn } from "@/lib/utils";
import { client } from "../control/client";
import { errorText, notifyError } from "../lib/flash";
import { useEditorStore } from "../state/store";
import type { AssetModelResult } from "../protocol";

function formatSeconds(sec: number): string {
  return `${(Number.isFinite(sec) && sec > 0 ? sec : 0).toFixed(2)}s`;
}

interface ClipListProps {
  model: AssetModelResult | null;
  rootEntity: string | null;
}

export function ClipList({ model, rootEntity }: ClipListProps) {
  // The previewed model is the selected entity, so the selection-keyed animationState slice mirrors it.
  const activeClip = useEditorStore((s) => s.animationState?.clip ?? null);
  const wrap = useEditorStore((s) => s.animationState?.wrap ?? null);
  const [pendingClip, setPendingClip] = useState<string | null>(null);
  const [focusClip, setFocusClip] = useState<string | null>(null);

  // Engine truth wins after one poll: clear the optimistic pick whenever the polled active clip moves.
  useEffect(() => {
    setPendingClip(null);
  }, [activeClip]);

  const clips = model?.clips ?? [];
  const selectedClip = pendingClip ?? activeClip;
  const focused = focusClip ? clips.find((clip) => clip.id === focusClip) : undefined;

  const pick = (clipId: string): void => {
    if (!rootEntity) {
      return;
    }
    setPendingClip(clipId);
    setFocusClip(clipId);
    void client.playAnimation(rootEntity, clipId, { paused: true }).catch((err: unknown) => {
      notifyError(errorText(err));
    });
  };

  return (
    <div className="flex h-full flex-col bg-background">
      {clips.length === 0 ? (
        <div className="flex flex-1 items-center justify-center p-3 text-center text-xs italic text-muted-foreground">
          No clips imported with this model. Re-import it to add them.
        </div>
      ) : (
        <ScrollArea className="min-h-0 flex-1">
          <div className="py-1">
            {clips.map((clip) => (
              <button
                key={clip.id}
                type="button"
                className={cn(
                  "flex w-full items-center justify-between gap-2 px-3 py-1 text-left text-xs",
                  clip.id === selectedClip
                    ? "bg-accent text-accent-foreground"
                    : "hover:bg-accent/40",
                )}
                onClick={() => pick(clip.id)}
              >
                <span className="truncate">{clip.name}</span>
                <span className="shrink-0 tabular-nums text-muted-foreground">
                  {formatSeconds(clip.duration)}
                </span>
              </button>
            ))}
          </div>
        </ScrollArea>
      )}
      <div className="border-t border-border px-3 py-2 text-xs">
        {focused ? (
          <dl className="space-y-1">
            <DetailRow label="Name" value={focused.name} />
            <DetailRow label="Duration" value={formatSeconds(focused.duration)} />
            <DetailRow label="Tracks" value={String(focused.tracks)} />
            {wrap ? <DetailRow label="Wrap" value={wrap} /> : null}
          </dl>
        ) : model ? (
          <dl className="space-y-1">
            <DetailRow label="Mesh" value={model.name} />
            <DetailRow label="Bones" value={String(model.bones.length)} />
            <DetailRow
              label="Joints"
              value={String(model.bones.filter((bone) => bone.joint).length)}
            />
            <DetailRow label="Clips" value={String(model.clips.length)} />
          </dl>
        ) : null}
      </div>
    </div>
  );
}

function DetailRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex items-center justify-between gap-2">
      <dt className="text-muted-foreground">{label}</dt>
      <dd className="truncate text-foreground">{value}</dd>
    </div>
  );
}
