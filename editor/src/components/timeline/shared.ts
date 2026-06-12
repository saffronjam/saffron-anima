/// Shared seam + helpers for the timeline, factored from TimelinePanel so the dock and the rig editor
/// can each mount the same Transport + Surface against a different target (scene selection vs preview
/// rig). Command sinks stay `client.*` (both targets are entities in the active scene — the commands
/// are identical); only the id + gate differ.
import { errorText, notifyError } from "../../lib/flash";
import type { AnimationClipDto, AnimationStateResult } from "../../protocol";

/// The single animation track's accent (one clip → one track row in v1). A teal that reads as the
/// "animation" type-color signal; per-channel/per-bone rows with their own accents defer.
export const TRACK_ACCENT = "#2dd4bf";
export const TRACK_HEADER_WIDTH = 140;
/// Step granularity for the step-back/-fwd buttons (seconds). 1/30 ≈ one 30fps sample.
export const STEP_SEC = 1 / 30;

/// What the timeline drives: the command target id, the polled state mirror (playhead/clip/wrap), the
/// clips for the Transport's clip Select (the dock; the rig editor passes [] and hides it — its clip
/// list panel owns picking), and the rig gate (dock derives it from the inspect components, the rig
/// editor from preview-active).
export interface TimelineTarget {
  entityId: string | null;
  state: AnimationStateResult | null;
  clips: AnimationClipDto[];
  enabled: boolean;
}

export function formatTime(sec: number): string {
  if (!Number.isFinite(sec) || sec < 0) {
    sec = 0;
  }
  const ms = Math.round(sec * 1000);
  return `${(ms / 1000).toFixed(2)}s`;
}

export async function guard(op: () => Promise<unknown>): Promise<void> {
  try {
    await op();
  } catch (err) {
    notifyError(errorText(err));
  }
}
