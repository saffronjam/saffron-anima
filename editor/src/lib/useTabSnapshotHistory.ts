/// A reusable per-tab undo/redo hook for an editable main tab whose canonical state is
/// a local JS model flushed to the engine through one apply command — the material
/// graph today, any future asset editor next. It records a `{ before, after }` snapshot
/// at each apply boundary and pushes it onto the active tab's history; undo/redo replay
/// the editor's own apply command with the prior/next snapshot, so the engine stays
/// unaware of undo. A consumer supplies three model-specific functions: `read` (current
/// snapshot), `write` (show + persist a model), `equals` (stable, normalized compare).
///
/// The hook owns the snapshot baseline and a replay guard that outlives the store's
/// `historyReplaying` flag — a debounced editor re-applies its model AFTER the replay's
/// await resolves, so the consumer calls `consumeReplay()` at its apply boundary to skip
/// that settle's persist + record.
import { useRef } from "react";
import { useEditorStore } from "../state/store";
import type { UndoableEdit } from "./undo";

export interface TabSnapshotOptions<M> {
  /// Produce the current canonical snapshot of the editor's local model.
  read(): M;
  /// Make the editor show `model` AND persist it to the engine (plus any side effects,
  /// e.g. a preview re-render). This is the single path a replay calls.
  write(model: M): Promise<unknown>;
  /// Stable, normalized equality so a no-op settle records nothing.
  equals(a: M, b: M): boolean;
  /// Entry label for the Edit menu / affordance.
  label: string;
  /// Optional id restored as selection context on replay.
  selectionId?: string;
}

export interface TabSnapshotHistory<M> {
  /// Seed the baseline after a load so the first real edit diffs against the loaded model.
  seed(model: M): void;
  /// Call at each apply/settle boundary. Diffs `current ?? read()` against the baseline;
  /// on a real change pushes one entry to this tab's history and advances the baseline.
  record(current?: M): void;
  /// True-and-clear: if an undo/redo replay is settling, returns true once and resets.
  /// The consumer calls this to skip its own persist + record for the replay settle.
  consumeReplay(): boolean;
}

export function useTabSnapshotHistory<M>(
  tabId: string,
  opts: TabSnapshotOptions<M>,
): TabSnapshotHistory<M> {
  const lastApplied = useRef<M | null>(null);
  const replaying = useRef(false);
  // Refs so the stable api closes over the latest opts (the consumer rebuilds `write`
  // each render) and the current tab id, without recreating the api object.
  const optsRef = useRef(opts);
  optsRef.current = opts;
  const tabIdRef = useRef(tabId);
  tabIdRef.current = tabId;

  const api = useRef<TabSnapshotHistory<M> | null>(null);
  if (api.current === null) {
    api.current = {
      seed(model: M): void {
        lastApplied.current = model;
      },
      record(current?: M): void {
        const o = optsRef.current;
        const after = current ?? o.read();
        const before = lastApplied.current;
        lastApplied.current = after;
        if (before === null || o.equals(before, after)) {
          return;
        }
        const edit: UndoableEdit = {
          label: o.label,
          selectionId: o.selectionId,
          undo: async () => {
            replaying.current = true;
            await optsRef.current.write(before);
            lastApplied.current = before;
          },
          redo: async () => {
            replaying.current = true;
            await optsRef.current.write(after);
            lastApplied.current = after;
          },
        };
        useEditorStore.getState().pushEdit(edit, tabIdRef.current);
      },
      consumeReplay(): boolean {
        if (replaying.current) {
          replaying.current = false;
          return true;
        }
        return false;
      },
    };
  }
  return api.current;
}
