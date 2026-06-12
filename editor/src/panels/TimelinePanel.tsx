/// The bottom-dock Timeline: a read-only, canvas-rendered sequencer for the selected rig. A thin
/// composition over the shared TimelineTransport + TimelineSurface (components/timeline/), built from
/// the dock's four store reads (selection, animation state/clips, inspect components for the rig gate).
/// The rig editor mounts the same pieces against the preview rig (a different TimelineTarget); the two
/// never render simultaneously (the dock is hidden while a non-scene tab is active).
///
/// Keyframe AUTHORING is out of scope; the lane renderer already has a `diamonds` mode so it drops in
/// later without a layout rewrite.
import { useEditorStore } from "../state/store";
import { TimelineTransport } from "../components/timeline/TimelineTransport";
import { TimelineSurface } from "../components/timeline/TimelineSurface";
import type { TimelineTarget } from "../components/timeline/shared";

/// A rig is an entity that carries a SkinnedMesh or AnimationPlayer component. `listClips` returns the
/// whole project catalog regardless of entity, so the clip list alone cannot gate the panel — without
/// this an unrigged cube would show a phantom track. The inspect result's component map (filled by the
/// reconcile poll on selection) is the reliable signal.
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

  const target: TimelineTarget = {
    entityId: selectedId,
    state: animationState,
    clips: animationClips,
    enabled: animationState !== null || isRiggedEntity(components),
  };

  return (
    <div className="flex h-full min-h-0 flex-col bg-background text-foreground">
      <TimelineTransport target={target} showClipSelect />
      <TimelineSurface target={target} />
    </div>
  );
}
