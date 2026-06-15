/// The timeline transport bar: jump/step/play-pause/loop + an optional clip Select. Target-agnostic —
/// it reads the polled state mirror from `target` and commands `target.entityId`. The dock shows the
/// clip Select; the asset editor hides it (its clip list panel owns picking). A move out of TimelinePanel
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
      void guard(() => client.setAnimationPlaying(entityId, false));
    } else if (activeClipId) {
      // Resume from the current playhead — play-animation would restart the clip at frame 0.
      void guard(() => client.setAnimationPlaying(entityId, true));
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

  const loopButton = (
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
  );

  const clipSelect =
    showClipSelect && enabled && clips.length > 0 ? (
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
    ) : null;

  // One layout for both mounts: a 3-column grid with equal 1fr gutters centers the playback group;
  // the loop toggle is absolutely anchored just past the group's right edge so it sits beside the
  // group without shifting its centering. The dock fills the left gutter with the clip Select; the
  // asset editor leaves it empty (its clip list owns picking) and the group stays centered all the same.
  return (
    <div className="grid h-9 flex-none grid-cols-[1fr_auto_1fr] items-center gap-2 border-b border-border px-2">
      <div className="flex min-w-0 items-center justify-self-start">{clipSelect}</div>
      <div
        className="relative flex items-center gap-0.5 justify-self-center"
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
        <div className="absolute left-full top-1/2 ml-2 flex -translate-y-1/2 items-center">
          {loopButton}
        </div>
      </div>
    </div>
  );
}
