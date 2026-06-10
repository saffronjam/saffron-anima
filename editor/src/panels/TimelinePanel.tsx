/// The bottom-dock Timeline: a read-only, canvas-rendered sequencer for the selected rig.
/// Track rows (left) / ms ruler (top) / clip bars (lanes) / a scrubbable playhead / transport
/// + a Loop toggle / a `Duration · N tracks · N clips` footer. It REFLECTS the engine's
/// animation player (the reconcile poll's animationVersion gate fills `animationState`) and
/// drives Edit-mode preview of the selection over the control plane.
///
/// Motion is imperative, never React state: the playhead/ruler/lanes live on one `TimelineCanvas`
/// fed on rAF (the `FrameTimeGraph` model) so the playhead advancing every poll never re-renders
/// the panel — the webview composites over the live viewport, so editor CPU is not free.
/// Keyframe AUTHORING is out of scope; the lane renderer already has a `diamonds` mode so it
/// drops in later without a layout rewrite.
import { useEffect, useMemo, useRef } from "react";
import {
  ChevronFirst,
  ChevronLast,
  Pause,
  Play,
  Repeat,
  StepBack,
  StepForward,
} from "lucide-react";
import { useEditorStore } from "../state/store";
import { client } from "../control/client";
import { makeCoalescer } from "../control/coalesce";
import { useScrubValue } from "../lib/useScrubValue";
import { errorText, notifyError } from "../lib/flash";
import {
  TimelineCanvas,
  type TimelineClip,
  type TimelineModel,
  type TimelineTrack,
} from "../lib/timelineCanvas";
import { Button } from "@/components/ui/button";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";
import { cn } from "@/lib/utils";

/// The single animation track's accent (one clip → one track row in v1). A teal that reads as
/// the "animation" type-color signal; per-channel/per-bone rows with their own accents defer.
const TRACK_ACCENT = "#2dd4bf";
const TRACK_HEADER_WIDTH = 140;
/// Step granularity for the step-back/-fwd buttons (seconds). 1/30 ≈ one 30fps sample.
const STEP_SEC = 1 / 30;

function formatTime(sec: number): string {
  if (!Number.isFinite(sec) || sec < 0) {
    sec = 0;
  }
  const ms = Math.round(sec * 1000);
  return `${(ms / 1000).toFixed(2)}s`;
}

async function guard(op: () => Promise<unknown>): Promise<void> {
  try {
    await op();
  } catch (err) {
    notifyError(errorText(err));
  }
}

/// A rig is an entity that carries a SkinnedMesh or AnimationPlayer component. `listClips`
/// returns the whole project catalog regardless of entity, so the clip list alone cannot
/// gate the panel — without this an unrigged cube would show a phantom track. The inspect
/// result's component map (filled by the reconcile poll on selection) is the reliable signal.
function isRiggedEntity(components: Record<string, unknown> | undefined): boolean {
  return (
    components !== undefined && ("AnimationPlayer" in components || "SkinnedMesh" in components)
  );
}

export function TimelinePanel() {
  const selectedId = useEditorStore((s) => s.selectedId);
  const animationState = useEditorStore((s) => s.animationState);
  const animationClips = useEditorStore((s) => s.animationClips);
  const components = useEditorStore(
    (s) => s.componentsBySelected?.components as Record<string, unknown> | undefined,
  );

  const hasRig = animationState !== null || isRiggedEntity(components);
  const playing = animationState?.playing ?? false;
  // The fallbacks to animationClips[0] only apply to a rig that has not been played yet (no
  // AnimationPlayer): show its first catalog clip's bar so Play has a target. A non-rigged
  // selection reads zero so the footer stays honest.
  const duration = hasRig ? (animationState?.duration ?? animationClips[0]?.duration ?? 0) : 0;
  const wrap = animationState?.wrap ?? "loop";
  const looping = wrap === "loop" || wrap === "pingpong";
  const activeClipId = animationState ? animationState.clip : (animationClips[0]?.id ?? "");
  const clipLabel =
    animationState?.clipName ||
    animationClips.find((c) => c.id === activeClipId)?.name ||
    animationClips[0]?.name ||
    "Clip";

  // One coalesced seek stream, ≤1 send in flight, latest value wins — correct for scrubbing
  // (intermediate frames are not critical). Stable across renders; reads selectedId via ref.
  const selectedRef = useRef<string | null>(selectedId);
  selectedRef.current = selectedId;
  const seekCoalescer = useMemo(
    () =>
      makeCoalescer<number>({
        throttleMs: 50,
        send: (t) => {
          const id = selectedRef.current;
          return id ? client.seekAnimation(id, t) : Promise.resolve();
        },
      }),
    [],
  );

  // The playhead's drag-local value: follows `animationState.time` outside a gesture, and the
  // pointer owns it during a scrub (emitting coalesced seeks). The canvas reads it imperatively.
  const playTime = animationState?.time ?? 0;
  const scrub = useScrubValue<number>(playTime, (t) => seekCoalescer.push(t));
  const draggingRef = useRef(false);

  const canvasHostRef = useRef<HTMLDivElement | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const headerHostRef = useRef<HTMLDivElement | null>(null);
  const engineRef = useRef<TimelineCanvas | null>(null);
  // A bridge so the pointer handlers (outside the init effect) can re-sync the playhead to the
  // store's `time` after a scrub ends — the resync closure is built inside the init effect.
  const hostApplyPlayheadRef = useRef<(() => void) | null>(null);

  // The track list React state — these CHANGE infrequently (only on selection/clip), so they
  // ride React; the playhead/ruler/bars do NOT (they live on the canvas).
  const tracks = useMemo<TimelineTrack[]>(
    () => (hasRig ? [{ id: "anim", accent: TRACK_ACCENT }] : []),
    [hasRig],
  );

  // Create the canvas ONCE (FrameTimeGraph pattern): init the engine, size it to the host, and
  // pump the model + playhead imperatively. Subscribing here means the playhead advancing on a
  // poll touches only the canvas, never a React render.
  useEffect(() => {
    const host = canvasHostRef.current;
    const canvas = canvasRef.current;
    if (!host || !canvas) {
      return;
    }
    const engine = new TimelineCanvas(canvas);
    engineRef.current = engine;

    const fit = (): void => {
      const rect = host.getBoundingClientRect();
      engine.setSize(
        Math.max(1, rect.width),
        Math.max(1, rect.height),
        window.devicePixelRatio || 1,
      );
    };
    fit();

    const applyModel = (): void => {
      const st = useEditorStore.getState();
      const rigged =
        st.animationState !== null ||
        isRiggedEntity(st.componentsBySelected?.components as Record<string, unknown> | undefined);
      const dur = rigged ? (st.animationState?.duration ?? st.animationClips[0]?.duration ?? 0) : 0;
      const trackList: TimelineTrack[] = rigged ? [{ id: "anim", accent: TRACK_ACCENT }] : [];
      const clips: TimelineClip[] = [];
      if (trackList.length > 0 && dur > 0) {
        clips.push({
          trackId: "anim",
          label: st.animationState?.clipName || st.animationClips[0]?.name || "Clip",
          start: 0,
          duration: dur,
        });
      }
      const model: TimelineModel = {
        duration: Math.max(dur, 0.0001),
        tracks: trackList,
        clips,
        keys: [],
        mode: "bars",
      };
      engine.setModel(model);
      // Re-sync the header row height styling to the canvas metrics on each model change.
      const header = headerHostRef.current;
      if (header) {
        header.style.setProperty("--ruler-h", `${engine.rulerHeight}px`);
        header.style.setProperty("--row-h", `${engine.rowHeight}px`);
      }
    };

    const applyPlayhead = (): void => {
      if (draggingRef.current) {
        return; // the pointer owns the playhead mid-scrub
      }
      const t = useEditorStore.getState().animationState?.time ?? 0;
      engine.setPlayhead(t);
    };

    applyModel();
    applyPlayhead();

    let lastState = useEditorStore.getState().animationState;
    let lastClips = useEditorStore.getState().animationClips;
    let lastComponents = useEditorStore.getState().componentsBySelected;
    const unsub = useEditorStore.subscribe((state) => {
      if (state.animationClips !== lastClips) {
        lastClips = state.animationClips;
        applyModel();
      }
      // The rig gate reads the inspect result; re-lay the track when it lands (selection change).
      if (state.componentsBySelected !== lastComponents) {
        lastComponents = state.componentsBySelected;
        applyModel();
      }
      if (state.animationState !== lastState) {
        const prev = lastState;
        lastState = state.animationState;
        // A clip/duration change re-lays the bars; otherwise only the playhead moves.
        if (
          !prev ||
          !state.animationState ||
          prev.clip !== state.animationState.clip ||
          prev.duration !== state.animationState.duration
        ) {
          applyModel();
        }
        applyPlayhead();
      }
    });

    const ro = new ResizeObserver(() => {
      fit();
      applyPlayhead();
    });
    ro.observe(host);

    // Expose the playhead resync for the scrub-end handler below.
    hostApplyPlayheadRef.current = applyPlayhead;

    return () => {
      unsub();
      ro.disconnect();
      engine.destroy();
      engineRef.current = null;
      hostApplyPlayheadRef.current = null;
    };
  }, []);

  // --- transport ---
  const onPlayPause = (): void => {
    if (!selectedId) {
      return;
    }
    if (playing) {
      void guard(() => client.pauseAnimation(selectedId));
    } else if (activeClipId) {
      void guard(() => client.playAnimation(selectedId, activeClipId, { loop: looping }));
    }
  };
  const onSeek = (t: number): void => {
    if (selectedId) {
      void guard(() => client.seekAnimation(selectedId, t));
    }
  };
  const onJumpStart = (): void => onSeek(0);
  const onJumpEnd = (): void => onSeek(duration);
  const onStepBack = (): void => onSeek(Math.max(0, playTime - STEP_SEC));
  const onStepFwd = (): void => onSeek(Math.min(duration, playTime + STEP_SEC));
  const onToggleLoop = (): void => {
    if (selectedId) {
      void guard(() => client.setAnimationLoop(selectedId, looping ? "once" : "loop"));
    }
  };
  const onPickClip = (clipId: string): void => {
    if (selectedId && clipId) {
      void guard(() => client.playAnimation(selectedId, clipId, { loop: looping }));
    }
  };

  // --- scrub ---
  const beginScrub = (clientX: number): void => {
    const engine = engineRef.current;
    const canvas = canvasRef.current;
    if (!engine || !canvas) {
      return;
    }
    draggingRef.current = true;
    scrub.begin();
    moveScrub(clientX);
  };
  const moveScrub = (clientX: number): void => {
    const engine = engineRef.current;
    const canvas = canvasRef.current;
    if (!engine || !canvas) {
      return;
    }
    const rect = canvas.getBoundingClientRect();
    const sec = engine.xToSec(clientX - rect.left);
    scrub.set(sec); // drag-local + coalesced seek
    engine.setPlayhead(sec); // imperative playhead, no React render
  };
  const endScrub = (): void => {
    scrub.end(); // FLUSH the final value before clearing the gesture
    draggingRef.current = false;
    hostApplyPlayheadRef.current?.();
  };

  const onPointerDown = (e: React.PointerEvent): void => {
    if (!hasRig) {
      return;
    }
    e.currentTarget.setPointerCapture(e.pointerId);
    beginScrub(e.clientX);
  };
  const onPointerMove = (e: React.PointerEvent): void => {
    if (draggingRef.current) {
      moveScrub(e.clientX);
    }
  };
  const onPointerUp = (e: React.PointerEvent): void => {
    if (draggingRef.current) {
      e.currentTarget.releasePointerCapture(e.pointerId);
      endScrub();
    }
  };

  const nTracks = tracks.length;
  const nClips = duration > 0 && nTracks > 0 ? 1 : 0;

  return (
    <div className="flex h-full min-h-0 flex-col bg-background text-foreground">
      {/* transport bar */}
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
            onClick={onJumpStart}
            disabled={!hasRig}
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
                onClick={onStepBack}
                disabled={!hasRig}
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
            disabled={!hasRig}
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
                onClick={onStepFwd}
                disabled={!hasRig}
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
            onClick={onJumpEnd}
            disabled={!hasRig}
            aria-label="Jump to end"
          >
            <ChevronLast />
          </Button>
        </div>

        {hasRig && animationClips.length > 0 ? (
          <Select
            value={typeof activeClipId === "string" ? activeClipId : String(activeClipId)}
            onValueChange={onPickClip}
          >
            <SelectTrigger size="sm" className="h-7 w-[160px] text-[11px]">
              <SelectValue placeholder="Clip" />
            </SelectTrigger>
            <SelectContent>
              {animationClips.map((c) => (
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
                disabled={!hasRig}
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

      {/* body: track headers (left) + lane canvas (right) */}
      <div className="flex min-h-0 flex-1">
        <div
          ref={headerHostRef}
          className="flex flex-none flex-col border-r border-border"
          style={{ width: TRACK_HEADER_WIDTH }}
        >
          <div
            className="flex flex-none items-center border-b border-border px-2 text-[10px] uppercase tracking-wide text-muted-foreground"
            style={{ height: "var(--ruler-h, 22px)" }}
          >
            Tracks
          </div>
          <div className="min-h-0 flex-1 overflow-hidden">
            {tracks.map((t) => (
              <div
                key={t.id}
                className="flex items-center gap-2 border-b border-border px-2 text-xs"
                style={{ height: "var(--row-h, 24px)" }}
              >
                <span
                  className="size-2.5 flex-none rounded-[2px]"
                  style={{ backgroundColor: t.accent }}
                />
                <span className="truncate text-foreground">{clipLabel}</span>
              </div>
            ))}
            {tracks.length === 0 ? (
              <div className="px-2 py-3 text-[11px] text-muted-foreground">
                {selectedId ? "No animation on this entity." : "Select a rigged entity."}
              </div>
            ) : null}
          </div>
        </div>

        <div ref={canvasHostRef} className="relative min-h-0 min-w-0 flex-1 overflow-hidden">
          <canvas ref={canvasRef} className="absolute inset-0 block" />
          {/* a full-area pointer surface so a click/drag anywhere scrubs (Premiere/UE style); the
              playhead line itself lives on the canvas, moved imperatively by the scrub handler */}
          {hasRig ? (
            <div
              className="absolute inset-0 cursor-ew-resize"
              onPointerDown={onPointerDown}
              onPointerMove={onPointerMove}
              onPointerUp={onPointerUp}
              onPointerCancel={onPointerUp}
            />
          ) : null}
        </div>
      </div>

      {/* footer: summary + time readout */}
      <div className="flex h-6 flex-none items-center justify-between border-t border-border px-2 text-[11px] text-muted-foreground">
        <span>
          Duration {formatTime(duration)} · {nTracks} {nTracks === 1 ? "track" : "tracks"} ·{" "}
          {nClips} {nClips === 1 ? "clip" : "clips"}
        </span>
        <span className={cn("font-mono", playing && "text-foreground")}>
          {formatTime(playTime)} / {formatTime(duration)}
        </span>
      </div>
    </div>
  );
}
