/// The timeline surface: track headers (left) + the canvas lanes + a full-area scrub surface + the
/// footer. Owns its own TimelineCanvas, scrub pipeline, and driving subscription (per-mount via
/// useMemo([]) / a mount effect — nothing module-level leaks between the dock and rig-editor mounts).
/// Motion stays imperative (the playhead advances on the canvas, not via React); the command target +
/// rig gate are injected through `target`. A move out of TimelinePanel — identical DOM/classes.
///
/// The canvas reads the store's animationState slice imperatively (the no-re-render playhead path);
/// both mounts share that slice because the relevant entity (scene selection / preview rig) is the
/// engine selection. The injected `enabled` gates the model so a hidden/parked mount stays empty.
import { useEffect, useMemo, useRef } from "react";
import { useEditorStore } from "../../state/store";
import { client } from "../../control/client";
import { makeCoalescer } from "../../control/coalesce";
import { useScrubValue } from "../../lib/useScrubValue";
import {
  TimelineCanvas,
  type TimelineClip,
  type TimelineModel,
  type TimelineTrack,
} from "../../lib/timelineCanvas";
import { cn } from "@/lib/utils";
import { TRACK_ACCENT, TRACK_HEADER_WIDTH, type TimelineTarget, formatTime } from "./shared";

export function TimelineSurface({ target }: { target: TimelineTarget }) {
  const { entityId, state, enabled } = target;

  // Injected values the imperative effect/scrub read without re-subscribing.
  const entityRef = useRef<string | null>(entityId);
  entityRef.current = entityId;
  const enabledRef = useRef<boolean>(enabled);
  enabledRef.current = enabled;

  // One coalesced seek stream, ≤1 send in flight, latest value wins — correct for scrubbing.
  const seekCoalescer = useMemo(
    () =>
      makeCoalescer<number>({
        throttleMs: 50,
        send: (t) => {
          const id = entityRef.current;
          return id ? client.seekAnimation(id, t) : Promise.resolve();
        },
      }),
    [],
  );

  const playTime = state?.time ?? 0;
  const duration = enabled ? (state?.duration ?? 0) : 0;
  const playing = state?.playing ?? false;
  const clipLabel = state?.clipName || "Clip";
  const scrub = useScrubValue<number>(playTime, (t) => seekCoalescer.push(t));
  const draggingRef = useRef(false);

  const canvasHostRef = useRef<HTMLDivElement | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const headerHostRef = useRef<HTMLDivElement | null>(null);
  const engineRef = useRef<TimelineCanvas | null>(null);
  const hostApplyPlayheadRef = useRef<(() => void) | null>(null);
  const hostRelayRef = useRef<(() => void) | null>(null);

  const tracks = useMemo<TimelineTrack[]>(
    () => (enabled ? [{ id: "anim", accent: TRACK_ACCENT }] : []),
    [enabled],
  );

  // Create the canvas ONCE: init the engine, size it to the host, and pump the model + playhead
  // imperatively. Subscribing here means the playhead advancing on a poll touches only the canvas.
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
      // The rig gate is the injected `enabled`; a hidden/parked or non-rig mount lays an empty model.
      const rigged = enabledRef.current;
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
      engine.setPlayhead(useEditorStore.getState().animationState?.time ?? 0);
    };

    applyModel();
    applyPlayhead();

    let lastState = useEditorStore.getState().animationState;
    let lastClips = useEditorStore.getState().animationClips;
    let lastComponents = useEditorStore.getState().componentsBySelected;
    const unsub = useEditorStore.subscribe((s) => {
      if (s.animationClips !== lastClips) {
        lastClips = s.animationClips;
        applyModel();
      }
      if (s.componentsBySelected !== lastComponents) {
        lastComponents = s.componentsBySelected;
        applyModel();
      }
      if (s.animationState !== lastState) {
        const prev = lastState;
        lastState = s.animationState;
        if (
          !prev ||
          !s.animationState ||
          prev.clip !== s.animationState.clip ||
          prev.duration !== s.animationState.duration
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
    hostApplyPlayheadRef.current = applyPlayhead;
    hostRelayRef.current = (): void => {
      applyModel();
      applyPlayhead();
    };

    return () => {
      unsub();
      ro.disconnect();
      engine.destroy();
      engineRef.current = null;
      hostApplyPlayheadRef.current = null;
      hostRelayRef.current = null;
    };
  }, []);

  // Re-lay the model + playhead when the injected gate flips (enter/exit, selection rig-ness): the
  // effect's applyModel reads enabledRef but only re-runs on a store change, so a gate flip that lands
  // without one (e.g. preview becoming active before the first poll) would otherwise show a stale lane.
  useEffect(() => {
    hostRelayRef.current?.();
  }, [enabled]);

  // --- scrub ---
  const beginScrub = (clientX: number): void => {
    const engine = engineRef.current;
    if (!engine) {
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
    if (!enabled) {
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
    <div className="flex min-h-0 flex-1 flex-col">
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
                {entityId ? "No animation on this entity." : "Select a rigged entity."}
              </div>
            ) : null}
          </div>
        </div>

        <div ref={canvasHostRef} className="relative min-h-0 min-w-0 flex-1 overflow-hidden">
          <canvas ref={canvasRef} className="absolute inset-0 block" />
          {enabled ? (
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
