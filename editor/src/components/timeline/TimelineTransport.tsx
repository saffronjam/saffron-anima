/// The timeline transport bar: jump/step/play-pause/loop + an optional clip Select. Target-agnostic —
/// it reads the polled state mirror from `target` and commands `target.entityId`. The dock shows the
/// clip Select; the rig editor hides it (its clip list panel owns picking). A move out of TimelinePanel
/// — identical DOM/classes.
import {
  ChevronFirst,
  ChevronLast,
  Pause,
  Play,
  Repeat,
  StepBack,
  StepForward,
} from "lucide-react";
import { client } from "../../control/client";
import { Button } from "@/components/ui/button";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";
import { STEP_SEC, type TimelineTarget, guard } from "./shared";

export function TimelineTransport({
  target,
  showClipSelect,
}: {
  target: TimelineTarget;
  showClipSelect: boolean;
}) {
  const { entityId, state, clips, enabled } = target;
  const playing = state?.playing ?? false;
  const duration = enabled ? (state?.duration ?? clips[0]?.duration ?? 0) : 0;
  const wrap = state?.wrap ?? "loop";
  const looping = wrap === "loop" || wrap === "pingpong";
  const playTime = state?.time ?? 0;
  const activeClipId = state ? state.clip : (clips[0]?.id ?? "");

  const onSeek = (t: number): void => {
    if (entityId) {
      void guard(() => client.seekAnimation(entityId, t));
    }
  };
  const onPlayPause = (): void => {
    if (!entityId) {
      return;
    }
    if (playing) {
      void guard(() => client.pauseAnimation(entityId));
    } else if (activeClipId) {
      void guard(() => client.playAnimation(entityId, String(activeClipId), { loop: looping }));
    }
  };
  const onToggleLoop = (): void => {
    if (entityId) {
      void guard(() => client.setAnimationLoop(entityId, looping ? "once" : "loop"));
    }
  };
  const onPickClip = (clipId: string): void => {
    if (entityId && clipId) {
      void guard(() => client.playAnimation(entityId, clipId, { loop: looping }));
    }
  };

  return (
    <div className="flex h-9 flex-none items-center gap-2 border-b border-border px-2">
      <div
        className="flex items-center gap-0.5 rounded-md border border-border p-0.5"
        role="group"
        aria-label="Transport"
      >
        <Button
          type="button"
          size="icon-sm"
          variant="ghost"
          onClick={() => onSeek(0)}
          disabled={!enabled}
          aria-label="Jump to start"
        >
          <ChevronFirst />
        </Button>
        <Tooltip>
          <TooltipTrigger asChild>
            <Button
              type="button"
              size="icon-sm"
              variant="ghost"
              onClick={() => onSeek(Math.max(0, playTime - STEP_SEC))}
              disabled={!enabled}
              aria-label="Step back"
            >
              <StepBack />
            </Button>
          </TooltipTrigger>
          <TooltipContent>Step back one sample</TooltipContent>
        </Tooltip>
        <Button
          type="button"
          size="icon-sm"
          variant={playing ? "default" : "ghost"}
          onClick={onPlayPause}
          disabled={!enabled}
          aria-pressed={playing}
          aria-label={playing ? "Pause" : "Play"}
        >
          {playing ? <Pause /> : <Play />}
        </Button>
        <Tooltip>
          <TooltipTrigger asChild>
            <Button
              type="button"
              size="icon-sm"
              variant="ghost"
              onClick={() => onSeek(Math.min(duration, playTime + STEP_SEC))}
              disabled={!enabled}
              aria-label="Step forward"
            >
              <StepForward />
            </Button>
          </TooltipTrigger>
          <TooltipContent>Step forward one sample</TooltipContent>
        </Tooltip>
        <Button
          type="button"
          size="icon-sm"
          variant="ghost"
          onClick={() => onSeek(duration)}
          disabled={!enabled}
          aria-label="Jump to end"
        >
          <ChevronLast />
        </Button>
      </div>

      {showClipSelect && enabled && clips.length > 0 ? (
        <Select
          value={typeof activeClipId === "string" ? activeClipId : String(activeClipId)}
          onValueChange={onPickClip}
        >
          <SelectTrigger size="sm" className="h-7 w-[160px] text-[11px]">
            <SelectValue placeholder="Clip" />
          </SelectTrigger>
          <SelectContent>
            {clips.map((c) => (
              <SelectItem key={String(c.id)} value={String(c.id)} className="text-[11px]">
                {c.name}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>
      ) : null}

      <div className="ml-auto flex items-center gap-2">
        <Tooltip>
          <TooltipTrigger asChild>
            <Button
              type="button"
              size="icon-sm"
              variant={looping ? "default" : "ghost"}
              onClick={onToggleLoop}
              disabled={!enabled}
              aria-pressed={looping}
              aria-label="Loop"
            >
              <Repeat />
            </Button>
          </TooltipTrigger>
          <TooltipContent>{looping ? "Looping (click to play once)" : "Loop"}</TooltipContent>
        </Tooltip>
      </div>
    </div>
  );
}
