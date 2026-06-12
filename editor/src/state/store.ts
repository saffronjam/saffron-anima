/// Central editor state (Zustand) plus the focus-gated reconcile poll that keeps
/// the slices in sync with the running engine. Data panels (phases 5-8) read the
/// slices this fills.
import { create } from "zustand";
import { client, type Client } from "../control/client";
import type { ProjectInfo } from "../control/client";
import { COMMANDS_BY_ID, isCommandId, type CommandId } from "../lib/keybindings";
import { routeAlarmToasts } from "../lib/alarmToasts";
import { resetScriptErrorToasts, routeScriptErrorToasts } from "../lib/scriptErrorToasts";
import { appendFrameSamples } from "../lib/frameSeries";
import type {
  ActiveAlarmDto,
  AlarmEventDto,
  AnimationClipDto,
  AnimationStateResult,
  AssetEntry,
  EntityListEntry,
  Environment,
  FrameHistoryDto,
  GizmoState,
  InspectResult,
  PerfConfigDto,
  ProfileCaptureDto,
  RenderPassTimingsDto,
  RenderStats,
} from "../protocol";

/// Cap on the retained alarm-log entries (the dashboard shows the most recent).
const ALARM_LOG_LIMIT = 200;
/// Frames requested from the engine per metrics poll — its full ring, so no frames are
/// missed between polls (the client dedups the overlap by frame index).
const FRAME_HISTORY_SAMPLES = 1000;
const METRICS_RANGE_STORAGE_KEY = "saffron.metricsRangeSec";
const METRICS_BUCKET_STORAGE_KEY = "saffron.metricsBucketMs";
const METRICS_WINDOW_LEGACY_KEY = "saffron.metricsWindowSec"; // migrated once into the range key
const METRICS_REFRESH_STORAGE_KEY = "saffron.metricsRefreshMs";
const CAPTURE_WINDOW_STORAGE_KEY = "saffron.captureWindowFrames";
const CAPTURE_STATS_STORAGE_KEY = "saffron.captureIncludeStats";

export type EnginePhase = "idle" | "starting" | "attaching" | "ready" | "error";
/// The active left-bottom dock tab. Tree rows switch it (the Environment sentinel
/// selects-and-switches); the tab strip itself stays clickable as before.
export type BottomTab = "inspector" | "environment" | "render";

/// The performance tools openable into the right sidebar from the Topbar wrench menu.
export type RightTool = "stats" | "profiler" | "material";

/// The tools openable into the bottom dock from the Topbar. A parallel, independent
/// slice to the right-tools model; `'timeline'` is the only member until more land.
export type BottomTool = "timeline";

/// The profiler capture lifecycle, mirrored from the engine's recorder. On-demand: a
/// capture is armed by a button, never polled on the metrics lane.
export type CaptureState = "idle" | "arming" | "recording" | "ready";
export type ViewTab =
  | { id: "scene"; kind: "scene"; title: "Scene"; closable: false }
  | { id: "flamegraph"; kind: "flamegraph"; title: "Flame graph"; closable: true }
  | { id: string; kind: "materialGraph"; materialId: string; title: string; closable: true }
  | {
      id: string;
      kind: "asset";
      assetId: string;
      title: string;
      assetType: AssetEntry["type"];
      closable: true;
    };

export interface EngineStatus {
  running: boolean;
  phase: EnginePhase;
  error?: string;
}

/// The editor's play-mode state, mirrored from the engine.
export type PlayState = "edit" | "playing" | "paused";

/// The Assets-grid shift anchor: the last clicked tile, folder or asset.
export type AssetSelectionAnchor = { kind: "asset" | "folder"; key: string } | null;

/// One Assets-grid tile in the body's render order (folders sorted first, then
/// assets) — the coordinate space for shift-range selection.
export interface AssetGridItem {
  kind: "asset" | "folder";
  key: string;
}

export interface EditorState {
  entities: EntityListEntry[];
  selectedId: string | null;
  selectedMaterialId: string | null; // the material asset open in the Material editor panel
  /// Hierarchy rows whose children are shown. Plain UI state, deliberately outside
  /// the sceneVersion/selectionVersion keying so a scene mutation never collapses
  /// the tree; setEntities prunes ids that vanished from the scene.
  expandedIds: Set<string>;
  bottomTab: BottomTab;
  sceneVersion: number;
  selectionVersion: number;
  componentsBySelected: InspectResult | null;
  assets: AssetEntry[];
  assetFolders: string[];
  /// Assets-grid selection (UI-only, like devMode/viewTabs). Tiles subscribe to
  /// their own membership (`(s) => s.selectedAssetIds.has(id)`), so a selection
  /// delta re-renders only the flipped tiles, never the grid.
  selectedAssetIds: Set<string>;
  selectedFolderPaths: Set<string>;
  /// The shift-range anchor: the last clicked grid tile, folder or asset.
  assetSelectionAnchor: AssetSelectionAnchor;
  /// True while the Assets-grid marquee is sweeping; gates the details overlay so
  /// it opens once on release instead of flickering per crossed tile.
  assetMarqueeActive: boolean;
  viewTabs: ViewTab[];
  activeViewTabId: string;
  /// Performance tools open in the right sidebar (order = tab order); empty ⇒ the sidebar is
  /// closed. Session-only — opened from the Topbar wrench menu.
  rightTools: RightTool[];
  activeRightTool: RightTool | null;
  /// Tools open in the bottom dock (order = tab order); empty ⇒ the dock is closed.
  /// Session-only — opened from the Topbar timeline button. Parallel to rightTools.
  bottomTools: BottomTool[];
  activeBottomTool: BottomTool | null;
  environment: Environment | null;
  renderStats: RenderStats | null;
  /// Performance-telemetry slices (phases 1-4), filled by the gated metrics poll only
  /// while the Stats tab is open (history/passes) or always (alarms, for the badge).
  perfConfig: PerfConfigDto | null;
  frameHistory: FrameHistoryDto | null;
  passTimings: RenderPassTimingsDto | null;
  activeAlarms: ActiveAlarmDto[];
  /// Append-only FIRING/RESOLVED log (bounded), newest last; drives the dashboard log.
  alarmLog: AlarmEventDto[];
  /// The frame-time graph's time RANGE in seconds — how far back to display (bounded by the
  /// client history ring). A persisted view preference.
  metricsRangeSec: number;
  /// The graph's WINDOW (bucket / group-by) interval in ms — samples within it are averaged
  /// into one plotted point. The smoothness knob (larger = smoother). Persisted.
  metricsBucketMs: number;
  /// How often the metrics lane fetches (ms); persisted, default 1000 (1/s). The badge,
  /// graph, numbers, and per-pass all refresh at this rate.
  metricsRefreshMs: number;
  /// Pause the metrics lane (freeze the dashboard). Session-only; alarms catch up on resume.
  metricsPaused: boolean;
  /// Profiler capture (phases 5-7), request-scoped and kept OUT of the metrics lane. The
  /// last completed capture, its lifecycle state, and the in-flight frame progress.
  captureState: CaptureState;
  captureProgress: { current: number; total: number };
  capture: ProfileCaptureDto | null;
  /// Window length to capture (frames); persisted preference, default 1 (single-frame snapshot).
  captureWindowFrames: number;
  /// Request pipeline-statistics (overdraw / cull / vertex-reuse) in the capture; persisted,
  /// default off (the heaviest mode). The engine drops it gracefully when unsupported.
  captureIncludeStats: boolean;
  /// The pass name highlighted across the Profiler sub-views (Phase 7 cross-highlight).
  selectedPass: string | null;
  project: ProjectInfo | null;
  /// Client-side reconcile-poll rate (Hz), an EMA over the actual tick interval.
  /// This is the WEBVIEW poll cadence, NOT the engine frame rate (the engine's own
  /// fps/frameMs/gpuMs ride on `render-stats`; the native viewport paints
  /// independently of this poll).
  pollRateHz: number;
  /// Webview repaint cadence measured with requestAnimationFrame.
  uiFrameRateHz: number;
  uiFrameMs: number;
  engineStatus: EngineStatus;
  dragActive: boolean;
  gizmo: GizmoState;
  /// Editor play mode, mirrored from the engine via the reconcile poll
  /// (get-selection carries playState/playVersion; the poll dedups on the version).
  /// The gizmo is hidden and save/load are locked while not "edit"; panels stay live
  /// (writes discard on stop).
  playState: PlayState;
  /// The selected rig's animation player state (playhead, clip, wrap, speed) or null
  /// when the selection is not an animation player. Read-only mirror of the engine,
  /// refreshed by the reconcile poll's animationVersion gate; drives the TimelinePanel.
  animationState: AnimationStateResult | null;
  /// The animation clips available to the selected rig (the project catalog). Fetched
  /// alongside `animationState` when the selection changes.
  animationClips: AnimationClipDto[];
  /// When true, the native viewport window is parked off-screen so an overlay
  /// (a modal dialog) can paint over the viewport rect — the reparented X11
  /// child always paints on top otherwise. The ViewportPanel reads this and skips
  /// gluing the native window to its div until it clears.
  viewportHidden: boolean;
  /// True while a native OS file dialog (Tauri `open`/`save`) is showing. These
  /// dialogs are not window-modal under the reparented-viewport setup, so the
  /// webview stays live; this flag is the app-side lock that stops a second dialog
  /// from being opened and greys the controls that would open one.
  nativeDialogOpen: boolean;
  /// Show the SELECTED entity's components as read-only leaf subrows in the
  /// hierarchy (sourced from componentsBySelected — never an extra inspect).
  /// A persisted view preference, default off so the outliner stays clean.
  showComponentSubrows: boolean;
  /// Hide skeleton joints (rows flagged `bone` by list-entities) in the outliner;
  /// their non-bone descendants re-anchor to the nearest visible ancestor. A
  /// persisted view preference like the subrows toggle.
  hideBones: boolean;
  /// One-shot "jump the Inspector to this component" signal set by a subrow click;
  /// the Inspector consumes and clears it. Never gated on the poll's versions.
  focusComponent: string | null;
  /// Keybinding OVERRIDES only (command id → key-string); commands absent here use
  /// their registry default. Hydrated from appdata/settings.json at startup and
  /// persisted back (deltas only) on every change.
  keyBindings: Record<string, string>;
  /// True while the Editor Settings modal is open; gates the global shortcut hook
  /// (the dialog holds focus on non-text elements, so the text-entry guard alone
  /// would let shortcuts fire underneath it).
  settingsOpen: boolean;
  /// Developer mode: a persisted, app-wide flag that gates experimental/diagnostic
  /// UI flows (e.g. the project menu's Reload Project). Toggled by the hidden gesture
  /// in the titlebar; read by any panel that wants a dev-only affordance.
  devMode: boolean;

  setEntities(entities: EntityListEntry[]): void;
  /// Optimistically rename one entity's hierarchy row between polls (inline rename).
  /// The reconcile poll's sceneVersion bump re-fetches the authoritative list.
  applyOptimisticEntityName(id: string, name: string): void;
  setSelectedId(selectedId: string | null): void;
  setSelectedMaterialId(id: string | null): void;
  selectEntity(id: string): void;
  toggleExpanded(id: string): void;
  setExpanded(id: string, expanded: boolean): void;
  setBottomTab(bottomTab: BottomTab): void;
  /// Engine-authoritative reparent (null detaches to root): optimistically relink the
  /// moved entity's parentId in place — selection untouched — and hold dragActive over
  /// the round trip so the poll cannot clobber the relink. A rejection falls back to
  /// the next authoritative list (set-parent bumps sceneVersion on success only).
  setParent(id: string, parentId: string | null): Promise<void>;
  setSceneVersion(sceneVersion: number): void;
  setSelectionVersion(selectionVersion: number): void;
  setComponentsBySelected(components: InspectResult | null): void;
  /// Overlay one component's full DTO onto the live inspect result between polls
  /// (optimistic write). The reconcile poll is gated off mid-drag, so this is the
  /// UI's truth until the next poll; a new selectionVersion drops it (poll re-inspects).
  applyOptimisticComponent(component: string, dto: object): void;
  setAssetList(assets: AssetEntry[], folders: string[]): void;
  /// Re-fetch the catalog from the engine into `assets`. Called by the reconcile
  /// poll on a sceneVersion change and eagerly after an import/rename.
  refreshAssets(): Promise<void>;
  /// Instantiate a model asset into the scene (the decoupled "import once, instance many"
  /// path), then refresh the catalog. Returns the new root entity id, or null on failure.
  instantiateModel(modelId: string, name?: string): Promise<string | null>;
  /// Extract an embedded sub-asset to a standalone file (keeping its id), then refresh.
  extractSubAsset(modelId: string, subAssetId: string): Promise<void>;
  /// Revert an extracted sub-asset to the embedded chunk, then refresh.
  clearExtraction(modelId: string, subAssetId: string): Promise<void>;
  /// Rescan assets/ and reconcile the catalog from disk, then refresh.
  scanAssets(): Promise<void>;
  /// Re-bake a model from its source (skip if unchanged), then refresh.
  reimportModel(modelId: string): Promise<void>;
  /// Click selection for Assets-grid tiles: plain replaces, toggle (ctrl/meta)
  /// flips membership, shift unions the anchor→key range along `gridOrder` (an
  /// absent anchor falls through to the plain/toggle path).
  selectAssetGridItem(
    kind: "asset" | "folder",
    key: string,
    modifiers: { shift: boolean; toggle: boolean },
    gridOrder: AssetGridItem[],
  ): void;
  /// Replace the grid selection outright (marquee sweep, drag-collapse, clear);
  /// the anchor becomes the last asset, else the last folder, else null.
  setAssetSelection(assetIds: string[], folderPaths: string[]): void;
  /// Drop selected assets that left the visible grid and selected folders that no
  /// longer exist (plus a dangling anchor); identity-stable when nothing changed.
  pruneAssetSelection(visibleAssets: AssetEntry[], folders: readonly string[]): void;
  /// Drop just-deleted assets from the selection eagerly, before the refresh.
  removeFromAssetSelection(assetIds: ReadonlySet<string>): void;
  /// Rewrite selected folder paths through a folder move (prefix rename).
  rewriteSelectedFolderPaths(rewrite: (path: string) => string): void;
  setAssetMarqueeActive(assetMarqueeActive: boolean): void;
  openAssetTab(asset: AssetEntry): void;
  /// Open (or focus) the Flame graph main tab.
  openFlameTab(): void;
  /// Open (or focus) the node-graph editor for a material as a main tab.
  openMaterialGraphTab(materialId: string): void;
  closeViewTab(id: string): void;
  setActiveViewTab(id: string): void;
  moveViewTab(id: string, index: number): void;
  /// Open (or focus) a performance tool in the right sidebar.
  openRightTool(tool: RightTool): void;
  /// Close a right-sidebar tool; reassigns the active tool (or closes the sidebar).
  closeRightTool(tool: RightTool): void;
  setActiveRightTool(tool: RightTool): void;
  /// Open (or focus) a tool in the bottom dock.
  openBottomTool(tool: BottomTool): void;
  /// Close a bottom-dock tool; reassigns the active tool (or closes the dock).
  closeBottomTool(tool: BottomTool): void;
  setActiveBottomTool(tool: BottomTool): void;
  setEnvironment(environment: Environment | null): void;
  /// Set the selected rig's animation state and available clips together (poll-driven).
  setAnimationState(state: AnimationStateResult | null, clips: AnimationClipDto[]): void;
  setRenderStats(renderStats: RenderStats | null): void;
  setPerfConfig(perfConfig: PerfConfigDto | null): void;
  setFrameHistory(frameHistory: FrameHistoryDto | null): void;
  setPassTimings(passTimings: RenderPassTimingsDto | null): void;
  setActiveAlarms(activeAlarms: ActiveAlarmDto[]): void;
  /// Append drained alarm events to the bounded log (newest last).
  appendAlarmEvents(events: AlarmEventDto[]): void;
  /// Set + persist the frame-time graph's time range (seconds).
  setMetricsRangeSec(metricsRangeSec: number): void;
  /// Set + persist the graph's bucket (group-by) interval (ms).
  setMetricsBucketMs(metricsBucketMs: number): void;
  /// Set + persist the metrics fetch interval (ms).
  setMetricsRefreshMs(metricsRefreshMs: number): void;
  /// Pause/resume the metrics lane.
  setMetricsPaused(metricsPaused: boolean): void;
  /// Profiler capture setters (request-scoped; no metrics-lane coupling).
  setCaptureState(captureState: CaptureState): void;
  setCaptureProgress(current: number, total: number): void;
  setCapture(capture: ProfileCaptureDto | null): void;
  /// Set + persist the capture window length (frames).
  setCaptureWindowFrames(captureWindowFrames: number): void;
  setCaptureIncludeStats(captureIncludeStats: boolean): void;
  setSelectedPass(selectedPass: string | null): void;
  setProject(project: ProjectInfo | null): void;
  setPollRateHz(pollRateHz: number): void;
  setUiFrameStats(frameRateHz: number, frameMs: number): void;
  /// Hard scene reset after a project/scene load: clear entities + selection +
  /// the live inspect result + assets + environment, invalidate cached thumbnails,
  /// and let the next reconcile tick re-fetch everything against the new scene
  /// (the engine cleared its own selection on load, so we mirror that). Idempotent.
  resetSceneState(): void;
  setEngineStatus(patch: Partial<EngineStatus>): void;
  setPhase(phase: EnginePhase, error?: string): void;
  setDragActive(dragActive: boolean): void;
  setGizmo(patch: Partial<GizmoState>): void;
  /// Optimistic play-state write (the reconcile poll repairs it from the engine).
  setPlayState(playState: PlayState): void;
  setViewportHidden(viewportHidden: boolean): void;
  setNativeDialogOpen(nativeDialogOpen: boolean): void;
  toggleComponentSubrows(): void;
  toggleHideBones(): void;
  setFocusComponent(focusComponent: string | null): void;
  /// Set one binding override and persist. A value equal to the registry default
  /// removes the override instead, keeping settings.json delta-minimal.
  setKeyBinding(id: CommandId, value: string): void;
  /// Remove one override (back to the default) and persist.
  resetKeyBinding(id: CommandId): void;
  /// Remove every override and persist.
  resetAllKeyBindings(): void;
  /// Load-time hydration: replace the override map (unknown command ids dropped),
  /// without writing back.
  hydrateKeyBindings(overrides: Record<string, string>): void;
  setSettingsOpen(settingsOpen: boolean): void;
  /// Set developer mode and persist it (localStorage, like the view-preference toggles).
  setDevMode(devMode: boolean): void;
}

export const useEditorStore = create<EditorState>((set) => ({
  entities: [],
  selectedId: null,
  selectedMaterialId: null,
  expandedIds: new Set<string>(),
  bottomTab: "inspector",
  sceneVersion: -1,
  selectionVersion: -1,
  componentsBySelected: null,
  assets: [],
  assetFolders: [],
  selectedAssetIds: new Set<string>(),
  selectedFolderPaths: new Set<string>(),
  assetSelectionAnchor: null,
  assetMarqueeActive: false,
  viewTabs: [{ id: "scene", kind: "scene", title: "Scene", closable: false }],
  activeViewTabId: "scene",
  rightTools: [],
  activeRightTool: null,
  bottomTools: [],
  activeBottomTool: null,
  environment: null,
  renderStats: null,
  perfConfig: null,
  frameHistory: null,
  passTimings: null,
  activeAlarms: [],
  alarmLog: [],
  metricsRangeSec: loadMetricsRangeSec(),
  metricsBucketMs: loadMetricsBucketMs(),
  metricsRefreshMs: loadMetricsRefreshMs(),
  metricsPaused: false,
  captureState: "idle",
  captureProgress: { current: 0, total: 0 },
  capture: null,
  captureWindowFrames: loadCaptureWindowFrames(),
  captureIncludeStats: loadCaptureIncludeStats(),
  selectedPass: null,
  project: null,
  pollRateHz: 0,
  uiFrameRateHz: 0,
  uiFrameMs: 0,
  engineStatus: { running: false, phase: "idle" },
  dragActive: false,
  gizmo: { op: "translate", space: "world", preserveChildren: false },
  playState: "edit",
  animationState: null,
  animationClips: [],
  viewportHidden: false,
  nativeDialogOpen: false,
  showComponentSubrows: loadShowSubrows(),
  hideBones: loadHideBones(),
  focusComponent: null,
  keyBindings: {},
  settingsOpen: false,
  devMode: loadDevMode(),

  setEntities: (entities) =>
    set((s) => {
      // Prune expand-state for ids that left the scene; survivors keep their state
      // so a poll refresh never collapses the tree.
      const present = new Set(entities.map((e) => e.id));
      let expandedIds = s.expandedIds;
      if ([...expandedIds].some((id) => !present.has(id))) {
        expandedIds = new Set([...expandedIds].filter((id) => present.has(id)));
        persistExpanded(expandedIds);
      }
      return { entities, expandedIds };
    }),
  applyOptimisticEntityName: (id, name) =>
    set((s) => ({
      entities: s.entities.map((e) => (e.id === id ? { ...e, name } : e)),
    })),
  setSelectedId: (selectedId) => set({ selectedId }),
  setSelectedMaterialId: (selectedMaterialId) => set({ selectedMaterialId }),
  // Optimistic local selection: set immediately on a hierarchy click so the row
  // highlights without waiting a poll interval; the reconcile poll confirms via
  // selectionVersion (engine is authoritative if a newer version arrives).
  selectEntity: (id) => set({ selectedId: id }),
  toggleExpanded: (id) =>
    set((s) => {
      const expandedIds = new Set(s.expandedIds);
      if (!expandedIds.delete(id)) {
        expandedIds.add(id);
      }
      persistExpanded(expandedIds);
      return { expandedIds };
    }),
  setExpanded: (id, expanded) =>
    set((s) => {
      if (s.expandedIds.has(id) === expanded) {
        return {};
      }
      const expandedIds = new Set(s.expandedIds);
      if (expanded) {
        expandedIds.add(id);
      } else {
        expandedIds.delete(id);
      }
      persistExpanded(expandedIds);
      return { expandedIds };
    }),
  setBottomTab: (bottomTab) => set({ bottomTab }),
  setParent: async (id, parentId) => {
    const previous = useEditorStore.getState().entities.find((e) => e.id === id)?.parentId;
    useEditorStore.getState().setDragActive(true);
    set((s) => ({
      entities: s.entities.map((e) =>
        e.id === id ? { ...e, parentId: parentId ?? undefined } : e,
      ),
    }));
    try {
      await client.setParent(id, parentId);
    } catch (err) {
      // A rejected reparent never bumps sceneVersion, so the poll will not restore
      // the row — roll the optimistic relink back by hand.
      set((s) => ({
        entities: s.entities.map((e) => (e.id === id ? { ...e, parentId: previous } : e)),
      }));
      throw err;
    } finally {
      useEditorStore.getState().setDragActive(false);
    }
  },
  setSceneVersion: (sceneVersion) => set({ sceneVersion }),
  setSelectionVersion: (selectionVersion) => set({ selectionVersion }),
  setComponentsBySelected: (componentsBySelected) => set({ componentsBySelected }),
  applyOptimisticComponent: (component, dto) =>
    set((s) => {
      if (!s.componentsBySelected) {
        return {};
      }
      return {
        componentsBySelected: {
          ...s.componentsBySelected,
          components: {
            ...s.componentsBySelected.components,
            [component]: dto,
          },
        },
      };
    }),
  setAssetList: (assets, assetFolders) =>
    set((s) => ({
      assets,
      assetFolders,
      viewTabs: s.viewTabs.map((tab) => {
        if (tab.kind !== "asset") {
          return tab;
        }
        const asset = assets.find((entry) => entry.id === tab.assetId);
        return asset ? { ...tab, title: asset.name, assetType: asset.type } : tab;
      }),
    })),
  refreshAssets: async () => {
    try {
      const list = await client.listAssets();
      useEditorStore.getState().setAssetList(list.assets, list.folders);
    } catch {
      // Engine may be briefly busy; the next reconcile tick recovers.
    }
  },
  // The decoupled-flow actions let a rejected control call propagate, so the calling panel
  // surfaces it through notifyError (the AGENTS toast rule); they refresh the catalog on success.
  instantiateModel: async (modelId, name) => {
    const entity = await client.instantiateModel(modelId, name);
    return entity.id ?? null;
  },
  extractSubAsset: async (modelId, subAssetId) => {
    await client.extractSubAsset(modelId, subAssetId);
    await useEditorStore.getState().refreshAssets();
  },
  clearExtraction: async (modelId, subAssetId) => {
    await client.clearExtraction(modelId, subAssetId);
    await useEditorStore.getState().refreshAssets();
  },
  scanAssets: async () => {
    await client.scanAssets();
    await useEditorStore.getState().refreshAssets();
  },
  reimportModel: async (modelId) => {
    await client.reimportModel(modelId);
    await useEditorStore.getState().refreshAssets();
  },
  selectAssetGridItem: (kind, key, modifiers, gridOrder) =>
    set((s) => {
      const index = gridOrder.findIndex((item) => item.kind === kind && item.key === key);
      if (index < 0) {
        return {};
      }
      const anchor = s.assetSelectionAnchor;
      if (modifiers.shift && anchor) {
        const anchorIndex = gridOrder.findIndex(
          (item) => item.kind === anchor.kind && item.key === anchor.key,
        );
        if (anchorIndex >= 0) {
          const range = gridOrder.slice(
            Math.min(anchorIndex, index),
            Math.max(anchorIndex, index) + 1,
          );
          const selectedAssetIds = new Set(s.selectedAssetIds);
          const selectedFolderPaths = new Set(s.selectedFolderPaths);
          for (const item of range) {
            (item.kind === "asset" ? selectedAssetIds : selectedFolderPaths).add(item.key);
          }
          return { selectedAssetIds, selectedFolderPaths, assetSelectionAnchor: { kind, key } };
        }
      }
      if (modifiers.toggle) {
        const next = new Set(kind === "asset" ? s.selectedAssetIds : s.selectedFolderPaths);
        if (!next.delete(key)) {
          next.add(key);
        }
        return {
          ...(kind === "asset" ? { selectedAssetIds: next } : { selectedFolderPaths: next }),
          assetSelectionAnchor: { kind, key },
        };
      }
      return {
        selectedAssetIds: kind === "asset" ? new Set([key]) : new Set<string>(),
        selectedFolderPaths: kind === "folder" ? new Set([key]) : new Set<string>(),
        assetSelectionAnchor: { kind, key },
      };
    }),
  setAssetSelection: (assetIds, folderPaths) =>
    set((s) => {
      if (
        assetIds.length === 0 &&
        folderPaths.length === 0 &&
        s.selectedAssetIds.size === 0 &&
        s.selectedFolderPaths.size === 0 &&
        s.assetSelectionAnchor === null
      ) {
        return {};
      }
      const lastAsset = assetIds.at(-1);
      const lastFolder = folderPaths.at(-1);
      return {
        selectedAssetIds: new Set(assetIds),
        selectedFolderPaths: new Set(folderPaths),
        assetSelectionAnchor: lastAsset
          ? { kind: "asset", key: lastAsset }
          : lastFolder
            ? { kind: "folder", key: lastFolder }
            : null,
      };
    }),
  pruneAssetSelection: (visibleAssets, folders) =>
    set((s) => {
      const visibleIds = new Set(visibleAssets.map((asset) => asset.id));
      const folderSet = new Set(folders);
      const patch: Partial<EditorState> = {};
      const assetIds = [...s.selectedAssetIds].filter((id) => visibleIds.has(id));
      if (assetIds.length !== s.selectedAssetIds.size) {
        patch.selectedAssetIds = new Set(assetIds);
      }
      const folderPaths = [...s.selectedFolderPaths].filter((path) => folderSet.has(path));
      if (folderPaths.length !== s.selectedFolderPaths.size) {
        patch.selectedFolderPaths = new Set(folderPaths);
      }
      const anchor = s.assetSelectionAnchor;
      if (
        anchor &&
        (anchor.kind === "asset" ? !visibleIds.has(anchor.key) : !folderSet.has(anchor.key))
      ) {
        patch.assetSelectionAnchor = null;
      }
      return patch;
    }),
  removeFromAssetSelection: (assetIds) =>
    set((s) => {
      const next = new Set([...s.selectedAssetIds].filter((id) => !assetIds.has(id)));
      return next.size === s.selectedAssetIds.size ? {} : { selectedAssetIds: next };
    }),
  rewriteSelectedFolderPaths: (rewrite) =>
    set((s) => {
      let changed = false;
      const next = new Set<string>();
      for (const path of s.selectedFolderPaths) {
        const to = rewrite(path);
        changed ||= to !== path;
        next.add(to);
      }
      return changed ? { selectedFolderPaths: next } : {};
    }),
  setAssetMarqueeActive: (assetMarqueeActive) => set({ assetMarqueeActive }),
  openAssetTab: (asset) =>
    set((s) => {
      const id = `asset:${asset.id}`;
      const existing = s.viewTabs.some((tab) => tab.id === id);
      return {
        activeViewTabId: id,
        viewTabs: existing
          ? s.viewTabs
          : [
              ...s.viewTabs,
              {
                id,
                kind: "asset",
                assetId: asset.id,
                title: asset.name,
                assetType: asset.type,
                closable: true,
              },
            ],
      };
    }),
  openFlameTab: () =>
    set((s) => {
      const existing = s.viewTabs.some((tab) => tab.id === "flamegraph");
      return {
        activeViewTabId: "flamegraph",
        viewTabs: existing
          ? s.viewTabs
          : [
              ...s.viewTabs,
              { id: "flamegraph", kind: "flamegraph", title: "Flame graph", closable: true },
            ],
      };
    }),
  openMaterialGraphTab: (materialId) =>
    set((s) => {
      const id = `materialGraph:${materialId}`;
      const existing = s.viewTabs.some((tab) => tab.id === id);
      return {
        activeViewTabId: id,
        viewTabs: existing
          ? s.viewTabs
          : [
              ...s.viewTabs,
              { id, kind: "materialGraph", materialId, title: "Material graph", closable: true },
            ],
      };
    }),
  closeViewTab: (id) =>
    set((s) => {
      if (id === "scene") {
        return {};
      }
      const index = s.viewTabs.findIndex((tab) => tab.id === id);
      const viewTabs = s.viewTabs.filter((tab) => tab.id !== id);
      const activeViewTabId =
        s.activeViewTabId === id
          ? (viewTabs[Math.max(0, index - 1)]?.id ?? "scene")
          : s.activeViewTabId;
      return { viewTabs, activeViewTabId };
    }),
  setActiveViewTab: (id) =>
    set((s) => (s.viewTabs.some((tab) => tab.id === id) ? { activeViewTabId: id } : {})),
  moveViewTab: (id, index) =>
    set((s) => {
      if (id === "scene") {
        return {};
      }
      const moving = s.viewTabs.find((tab) => tab.id === id);
      if (!moving) {
        return {};
      }
      const without = s.viewTabs.filter((tab) => tab.id !== id);
      const nextIndex = Math.min(Math.max(1, index), without.length);
      return {
        viewTabs: [...without.slice(0, nextIndex), moving, ...without.slice(nextIndex)],
      };
    }),
  openRightTool: (tool) =>
    set((s) => ({
      rightTools: s.rightTools.includes(tool) ? s.rightTools : [...s.rightTools, tool],
      activeRightTool: tool,
    })),
  closeRightTool: (tool) =>
    set((s) => {
      const index = s.rightTools.indexOf(tool);
      if (index < 0) {
        return {};
      }
      const rightTools = s.rightTools.filter((t) => t !== tool);
      const activeRightTool =
        s.activeRightTool === tool
          ? (rightTools[Math.max(0, index - 1)] ?? null)
          : s.activeRightTool;
      return { rightTools, activeRightTool };
    }),
  setActiveRightTool: (tool) =>
    set((s) => (s.rightTools.includes(tool) ? { activeRightTool: tool } : {})),
  openBottomTool: (tool) =>
    set((s) => ({
      bottomTools: s.bottomTools.includes(tool) ? s.bottomTools : [...s.bottomTools, tool],
      activeBottomTool: tool,
    })),
  closeBottomTool: (tool) =>
    set((s) => {
      const index = s.bottomTools.indexOf(tool);
      if (index < 0) {
        return {};
      }
      const bottomTools = s.bottomTools.filter((t) => t !== tool);
      const activeBottomTool =
        s.activeBottomTool === tool
          ? (bottomTools[Math.max(0, index - 1)] ?? null)
          : s.activeBottomTool;
      return { bottomTools, activeBottomTool };
    }),
  setActiveBottomTool: (tool) =>
    set((s) => (s.bottomTools.includes(tool) ? { activeBottomTool: tool } : {})),
  setEnvironment: (environment) => set({ environment }),
  setAnimationState: (animationState, animationClips) => set({ animationState, animationClips }),
  setRenderStats: (renderStats) => set({ renderStats }),
  setPerfConfig: (perfConfig) => set({ perfConfig }),
  setFrameHistory: (frameHistory) => set({ frameHistory }),
  setPassTimings: (passTimings) => set({ passTimings }),
  setActiveAlarms: (activeAlarms) => set({ activeAlarms }),
  appendAlarmEvents: (events) =>
    set((s) => {
      if (events.length === 0) {
        return {};
      }
      const alarmLog = [...s.alarmLog, ...events];
      return { alarmLog: alarmLog.slice(Math.max(0, alarmLog.length - ALARM_LOG_LIMIT)) };
    }),
  setMetricsRangeSec: (metricsRangeSec) =>
    set(() => {
      try {
        localStorage.setItem(METRICS_RANGE_STORAGE_KEY, String(metricsRangeSec));
      } catch {
        // Storage unavailable; the preference is then session-only.
      }
      return { metricsRangeSec };
    }),
  setMetricsBucketMs: (metricsBucketMs) =>
    set(() => {
      try {
        localStorage.setItem(METRICS_BUCKET_STORAGE_KEY, String(metricsBucketMs));
      } catch {
        // Storage unavailable; the preference is then session-only.
      }
      return { metricsBucketMs };
    }),
  setMetricsRefreshMs: (metricsRefreshMs) =>
    set(() => {
      try {
        localStorage.setItem(METRICS_REFRESH_STORAGE_KEY, String(metricsRefreshMs));
      } catch {
        // Storage unavailable; the preference is then session-only.
      }
      return { metricsRefreshMs };
    }),
  setMetricsPaused: (metricsPaused) => set({ metricsPaused }),
  setCaptureState: (captureState) => set({ captureState }),
  setCaptureProgress: (current, total) => set({ captureProgress: { current, total } }),
  setCapture: (capture) => set({ capture }),
  setCaptureWindowFrames: (captureWindowFrames) =>
    set(() => {
      try {
        localStorage.setItem(CAPTURE_WINDOW_STORAGE_KEY, String(captureWindowFrames));
      } catch {
        // Storage unavailable; the preference is then session-only.
      }
      return { captureWindowFrames };
    }),
  setCaptureIncludeStats: (captureIncludeStats) =>
    set(() => {
      try {
        localStorage.setItem(CAPTURE_STATS_STORAGE_KEY, captureIncludeStats ? "1" : "0");
      } catch {
        // Storage unavailable; the preference is then session-only.
      }
      return { captureIncludeStats };
    }),
  setSelectedPass: (selectedPass) => set({ selectedPass }),
  // Rehydrate the persisted expand-state when a project (path) becomes current.
  setProject: (project) =>
    set({
      project,
      expandedIds: project?.path ? loadExpanded(project.path) : new Set<string>(),
    }),
  setPollRateHz: (pollRateHz) => set({ pollRateHz }),
  setUiFrameStats: (uiFrameRateHz, uiFrameMs) => set({ uiFrameRateHz, uiFrameMs }),
  resetSceneState: () => {
    // The catalog changed under us, so every cached thumbnail blob URL is stale.
    invalidateThumbnails();
    set({
      entities: [],
      selectedId: null,
      expandedIds: new Set<string>(),
      componentsBySelected: null,
      assets: [],
      assetFolders: [],
      selectedAssetIds: new Set<string>(),
      selectedFolderPaths: new Set<string>(),
      assetSelectionAnchor: null,
      assetMarqueeActive: false,
      viewTabs: [{ id: "scene", kind: "scene", title: "Scene", closable: false }],
      activeViewTabId: "scene",
      environment: null,
      // Force the reconcile poll's version diff to fire on the next tick so the
      // hierarchy/inspector/assets/env all re-fetch against the loaded scene.
      sceneVersion: -1,
      selectionVersion: -1,
      // One-shot UI intent; never meaningful across a scene swap. The subrows
      // toggle survives — it is a view preference, not scene state.
      focusComponent: null,
    });
  },
  setEngineStatus: (patch) => set((s) => ({ engineStatus: { ...s.engineStatus, ...patch } })),
  setPhase: (phase, error) =>
    set((s) => ({
      engineStatus: {
        ...s.engineStatus,
        phase,
        error: phase === "error" ? error : undefined,
        running: phase === "ready" || phase === "error" ? s.engineStatus.running : phase !== "idle",
      },
    })),
  setDragActive: (dragActive) => set({ dragActive }),
  // Keeps the object identity when the patch changes nothing, so the reconcile
  // poll confirming an unchanged gizmo doesn't re-render every subscriber.
  setGizmo: (patch) =>
    set((s) => {
      const gizmo = { ...s.gizmo, ...patch };
      return gizmo.op === s.gizmo.op &&
        gizmo.space === s.gizmo.space &&
        gizmo.preserveChildren === s.gizmo.preserveChildren
        ? {}
        : { gizmo };
    }),
  setPlayState: (playState) => set({ playState }),
  setViewportHidden: (viewportHidden) => set({ viewportHidden }),
  setNativeDialogOpen: (nativeDialogOpen) => set({ nativeDialogOpen }),
  toggleComponentSubrows: () =>
    set((s) => {
      const showComponentSubrows = !s.showComponentSubrows;
      try {
        localStorage.setItem(SUBROWS_STORAGE_KEY, showComponentSubrows ? "1" : "0");
      } catch {
        // Storage unavailable; the preference is then session-only.
      }
      return { showComponentSubrows };
    }),
  toggleHideBones: () =>
    set((s) => {
      const hideBones = !s.hideBones;
      try {
        localStorage.setItem(HIDE_BONES_STORAGE_KEY, hideBones ? "1" : "0");
      } catch {
        // Storage unavailable; the preference is then session-only.
      }
      return { hideBones };
    }),
  setFocusComponent: (focusComponent) => set({ focusComponent }),
  setKeyBinding: (id, value) =>
    set((s) => {
      const keyBindings = { ...s.keyBindings };
      if (value === COMMANDS_BY_ID[id].default) {
        delete keyBindings[id];
      } else {
        keyBindings[id] = value;
      }
      persistKeyBindings(keyBindings);
      return { keyBindings };
    }),
  resetKeyBinding: (id) =>
    set((s) => {
      if (!(id in s.keyBindings)) {
        return {};
      }
      const keyBindings = { ...s.keyBindings };
      delete keyBindings[id];
      persistKeyBindings(keyBindings);
      return { keyBindings };
    }),
  resetAllKeyBindings: () => {
    persistKeyBindings({});
    set({ keyBindings: {} });
  },
  hydrateKeyBindings: (overrides) =>
    set({
      keyBindings: Object.fromEntries(Object.entries(overrides).filter(([id]) => isCommandId(id))),
    }),
  setSettingsOpen: (settingsOpen) => set({ settingsOpen }),
  setDevMode: (devMode) =>
    set(() => {
      try {
        localStorage.setItem(DEV_MODE_STORAGE_KEY, devMode ? "1" : "0");
      } catch {
        // Storage unavailable; the flag is then session-only.
      }
      return { devMode };
    }),
}));

/// Fire-and-forget settings write; the in-memory state stays applied on rejection
/// (the global shortcut hook has no panel to flash, so the failure goes to the console).
function persistKeyBindings(keyBindings: Record<string, string>): void {
  void client.saveEditorSettings({ keyBindings }).catch((err: unknown) => {
    console.error("save editor settings rejected:", err);
  });
}

/// Hydrate the keybinding overrides from appdata/settings.json (called once at app
/// start). A missing or unreadable file leaves the registry defaults active.
export async function loadEditorSettings(): Promise<void> {
  try {
    const settings = await client.loadEditorSettings();
    useEditorStore.getState().hydrateKeyBindings(settings.keyBindings ?? {});
  } catch {
    // Defaults stay active.
  }
}

/// Run a native file-dialog thunk (`open`/`save`) under the app-side dialog lock:
/// no-op if one is already open (the re-entry guard that stops stacked pickers),
/// otherwise hold `nativeDialogOpen` for the dialog's lifetime so the controls that
/// spawn dialogs grey out. The lock covers only the dialog, not the engine work that
/// follows it. Returns the thunk's result, or null when skipped by the guard.
export async function withNativeDialog<T>(fn: () => Promise<T>): Promise<T | null> {
  const store = useEditorStore.getState();
  if (store.nativeDialogOpen) {
    return null;
  }
  store.setNativeDialogOpen(true);
  try {
    return await fn();
  } finally {
    useEditorStore.getState().setNativeDialogOpen(false);
  }
}

/// One outliner node: an entity plus its resolved children. The tree is built
/// client-side from the flat `entities` slice; the engine ships only `parentId`.
export interface TreeNode {
  entity: EntityListEntry;
  children: TreeNode[];
}

/// Group the flat entity list into a forest by parentId. Absent/`"0"` parentId,
/// an unknown parent id, or a self-reference all land the entry at the root
/// (defensive: the engine roots dangling parents on load, and the client must
/// never loop). Sibling order preserves the engine's array order (unordered v1).
export function buildTree(entities: EntityListEntry[]): TreeNode[] {
  const nodes = new Map<string, TreeNode>();
  for (const entity of entities) {
    nodes.set(entity.id, { entity, children: [] });
  }
  const roots: TreeNode[] = [];
  for (const entity of entities) {
    const node = nodes.get(entity.id)!;
    const parentId = entity.parentId;
    const parent =
      parentId && parentId !== "0" && parentId !== entity.id ? nodes.get(parentId) : undefined;
    if (parent) {
      parent.children.push(node);
    } else {
      roots.push(node);
    }
  }
  return roots;
}

/// Expand-state persistence, keyed by project path (plain UI state, like the
/// thumbnail cache kept out of Zustand): survives reloads, never the poll's concern.
const EXPANDED_STORAGE_PREFIX = "saffron.expandedIds:";

function persistExpanded(ids: Set<string>): void {
  const path = useEditorStore.getState().project?.path;
  if (!path) {
    return;
  }
  try {
    localStorage.setItem(EXPANDED_STORAGE_PREFIX + path, JSON.stringify([...ids]));
  } catch {
    // Storage may be unavailable (private mode); expansion is then session-only.
  }
}

function loadExpanded(path: string): Set<string> {
  try {
    const raw = localStorage.getItem(EXPANDED_STORAGE_PREFIX + path);
    if (raw) {
      return new Set(JSON.parse(raw) as string[]);
    }
  } catch {
    // Fall through to empty.
  }
  return new Set<string>();
}

/// Sidebar-width persistence, keyed by project path like the expand-state (the
/// dock split ratios persist separately via react-resizable-panels' useDefaultLayout).
const SIDEBAR_WIDTH_STORAGE_PREFIX = "saffron.layout.sidebarWidth:";

export function persistSidebarWidth(path: string | undefined, width: number): void {
  if (!path) {
    return;
  }
  try {
    localStorage.setItem(SIDEBAR_WIDTH_STORAGE_PREFIX + path, String(Math.round(width)));
  } catch {
    // Storage may be unavailable (private mode); the width is then session-only.
  }
}

export function loadSidebarWidth(path: string | undefined): number | null {
  if (!path) {
    return null;
  }
  try {
    const raw = localStorage.getItem(SIDEBAR_WIDTH_STORAGE_PREFIX + path);
    if (raw !== null) {
      const width = Number(raw);
      if (Number.isFinite(width)) {
        return width;
      }
    }
  } catch {
    // Fall through to the default.
  }
  return null;
}

/// Right-sidebar (perf tools) width persistence, mirroring the left sidebar.
const RIGHT_SIDEBAR_WIDTH_STORAGE_PREFIX = "saffron.layout.rightSidebarWidth:";

export function persistRightSidebarWidth(path: string | undefined, width: number): void {
  if (!path) {
    return;
  }
  try {
    localStorage.setItem(RIGHT_SIDEBAR_WIDTH_STORAGE_PREFIX + path, String(Math.round(width)));
  } catch {
    // Storage may be unavailable (private mode); the width is then session-only.
  }
}

export function loadRightSidebarWidth(path: string | undefined): number | null {
  if (!path) {
    return null;
  }
  try {
    const raw = localStorage.getItem(RIGHT_SIDEBAR_WIDTH_STORAGE_PREFIX + path);
    if (raw !== null) {
      const width = Number(raw);
      if (Number.isFinite(width)) {
        return width;
      }
    }
  } catch {
    // Fall through to the default.
  }
  return null;
}

/// Bottom-dock (timeline) height persistence, mirroring the right-sidebar width.
const BOTTOM_DOCK_HEIGHT_STORAGE_PREFIX = "saffron.layout.bottomDockHeight:";

export function persistBottomDockHeight(path: string | undefined, height: number): void {
  if (!path) {
    return;
  }
  try {
    localStorage.setItem(BOTTOM_DOCK_HEIGHT_STORAGE_PREFIX + path, String(Math.round(height)));
  } catch {
    // Storage may be unavailable (private mode); the height is then session-only.
  }
}

export function loadBottomDockHeight(path: string | undefined): number | null {
  if (!path) {
    return null;
  }
  try {
    const raw = localStorage.getItem(BOTTOM_DOCK_HEIGHT_STORAGE_PREFIX + path);
    if (raw !== null) {
      const height = Number(raw);
      if (Number.isFinite(height)) {
        return height;
      }
    }
  } catch {
    // Fall through to the default.
  }
  return null;
}

/// The component-subrows toggle is a view preference (one key, unlike the
/// per-project expand-state); default off keeps the outliner clean.
const SUBROWS_STORAGE_KEY = "saffron.showComponentSubrows";

function loadShowSubrows(): boolean {
  try {
    return localStorage.getItem(SUBROWS_STORAGE_KEY) === "1";
  } catch {
    return false;
  }
}

const HIDE_BONES_STORAGE_KEY = "saffron.hideBones";

function loadHideBones(): boolean {
  try {
    return localStorage.getItem(HIDE_BONES_STORAGE_KEY) === "1";
  } catch {
    return false;
  }
}

/// Developer mode persists app-wide (one key, not per-project), default off.
/// `VITE_SAFFRON_DEV_MODE=1` (set by `make run-debug`) forces it on for the session
/// without touching the persisted flag.
const DEV_MODE_STORAGE_KEY = "saffron.devMode";

function loadDevMode(): boolean {
  if (import.meta.env.VITE_SAFFRON_DEV_MODE === "1") {
    return true;
  }
  try {
    return localStorage.getItem(DEV_MODE_STORAGE_KEY) === "1";
  } catch {
    return false;
  }
}

/// The frame-time graph time range (seconds); default 30 s. Migrates once from the legacy
/// "window" key (which was really the range) so an existing preference carries over.
function loadMetricsRangeSec(): number {
  try {
    const raw =
      localStorage.getItem(METRICS_RANGE_STORAGE_KEY) ??
      localStorage.getItem(METRICS_WINDOW_LEGACY_KEY);
    if (raw !== null) {
      const value = Number(raw);
      if (Number.isFinite(value) && value > 0) {
        return value;
      }
    }
  } catch {
    // Fall through to the default.
  }
  return 30;
}

/// The graph bucket (group-by) interval (ms); default 250. Clamped to a sane [10, 5000].
function loadMetricsBucketMs(): number {
  try {
    const raw = localStorage.getItem(METRICS_BUCKET_STORAGE_KEY);
    if (raw !== null) {
      const value = Number(raw);
      if (Number.isFinite(value) && value >= 10 && value <= 5000) {
        return value;
      }
    }
  } catch {
    // Fall through to the default.
  }
  return 250;
}

/// The metrics fetch interval (ms); default 1000 (1/s).
function loadMetricsRefreshMs(): number {
  try {
    const raw = localStorage.getItem(METRICS_REFRESH_STORAGE_KEY);
    if (raw !== null) {
      const value = Number(raw);
      if (Number.isFinite(value) && value >= 100) {
        return value;
      }
    }
  } catch {
    // Fall through to the default.
  }
  return 1000;
}

/// The capture window length (frames); default 1 (single-frame snapshot). Presets are
/// 1/8/64/256, clamped to the engine's [1, 256] capture cap.
function loadCaptureWindowFrames(): number {
  try {
    const raw = localStorage.getItem(CAPTURE_WINDOW_STORAGE_KEY);
    if (raw !== null) {
      const value = Number(raw);
      if (Number.isFinite(value) && value >= 1 && value <= 256) {
        return Math.floor(value);
      }
    }
  } catch {
    // Fall through to the default.
  }
  return 1;
}

/// Whether captures request pipeline statistics; default off (the heaviest mode).
function loadCaptureIncludeStats(): boolean {
  try {
    return localStorage.getItem(CAPTURE_STATS_STORAGE_KEY) === "1";
  } catch {
    return false;
  }
}

/// The outliner's bone filter: drop rows flagged `bone` and re-anchor every surviving
/// entity whose ancestry passes through bones to its nearest visible ancestor (walks
/// are bounded so corrupt data cannot loop). Pure — used before buildTree.
export function reanchorPastBones(entities: EntityListEntry[]): EntityListEntry[] {
  const byId = new Map(entities.map((e) => [e.id, e]));
  return entities
    .filter((e) => !e.bone)
    .map((e) => {
      let parent = e.parentId ? byId.get(e.parentId) : undefined;
      for (let steps = 0; parent?.bone && steps <= entities.length; steps++) {
        parent = parent.parentId ? byId.get(parent.parentId) : undefined;
      }
      const parentId = parent?.id;
      return parentId === e.parentId ? e : { ...e, parentId };
    });
}

const FAST_RECONCILE_INTERVAL_MS = 50; // cheap state lane, target ~20 Hz
const WATCHDOG_INTERVAL_MS = 1000;
// The metrics lane wakes on this base tick and fetches when `metricsRefreshMs` has elapsed
// (and not paused), so a rate or pause change applies within one tick instead of waiting
// out a long interval.
const METRICS_BASE_TICK_MS = 100;

/// Start the focus-gated reconcile loops. Cheap interactive state runs frequently;
/// heavier scene/entity/inspect refreshes only run when versions change and never
/// block the next cheap tick.
export function startReconcile(client: Client): () => void {
  let stopped = false;
  let fastTimer: ReturnType<typeof setTimeout> | null = null;
  let watchdogTimer: ReturnType<typeof setInterval> | null = null;
  let metricsTimer: ReturnType<typeof setTimeout> | null = null;
  let fastInFlight = false;
  let refreshInFlight = false;
  let watchdogInFlight = false;
  let metricsInFlight = false;
  let lastMetricsFetchAt = 0;
  // Alarm cursor (Last-Event-ID): only advances, so a missed poll just catches up.
  let alarmSince = 0;
  // Script-error cursor, same protocol; the toast dedup resets per play session.
  let scriptErrorSince = 0;
  let lastPlayStateForScripts: PlayState = "edit";
  let pendingRefresh: {
    selectedId: string | null;
    sceneChanged: boolean;
    selectionChanged: boolean;
    sceneVersion: number;
    previousSceneVersion: number;
  } | null = null;

  let knownSceneVersion = -1;
  let knownSelectionVersion = -1;
  let knownSelectedId: string | null = null;
  let knownPlayVersion = -1;
  let knownAnimationVersion = -1;
  let animationInFlight = false;

  let lastTickAt = 0;
  let emaIntervalMs = 0;

  const schedule = (): void => {
    if (stopped) {
      return;
    }
    fastTimer = setTimeout(tick, FAST_RECONCILE_INTERVAL_MS);
  };

  const readyForSync = (): boolean => {
    const store = useEditorStore.getState();
    return store.engineStatus.phase === "ready" && document.hasFocus();
  };

  /// The crash watchdog runs once the engine is live (socket bound) and stops once
  /// it has died or never started. This includes the starting/attaching window —
  /// the gap between socket-up and the viewport attach — so a child that binds its
  /// socket and then exits before attaching is still caught (the ViewportPanel's
  /// phase flip to ready never fires in that case).
  const engineMayBeAlive = (): boolean => {
    const phase = useEditorStore.getState().engineStatus.phase;
    return phase !== "idle" && phase !== "error" && document.hasFocus();
  };

  const refreshHeavyState = (request: {
    selectedId: string | null;
    sceneChanged: boolean;
    selectionChanged: boolean;
    sceneVersion: number;
    previousSceneVersion: number;
  }): void => {
    const { selectedId, sceneChanged, selectionChanged, sceneVersion, previousSceneVersion } =
      request;
    if (refreshInFlight || (!sceneChanged && !selectionChanged)) {
      if (refreshInFlight) {
        pendingRefresh = request;
      }
      return;
    }
    refreshInFlight = true;

    void (async () => {
      try {
        if (sceneChanged) {
          if (sceneVersion < previousSceneVersion) {
            invalidateThumbnails();
          }
          void useEditorStore.getState().refreshAssets();
          client
            .getEnvironment()
            .then((env) => {
              if (!stopped && !useEditorStore.getState().dragActive) {
                useEditorStore.getState().setEnvironment(env);
              }
            })
            .catch(() => {});

          const list = await client.listEntities();
          if (stopped) {
            return;
          }
          if (!useEditorStore.getState().dragActive) {
            useEditorStore.getState().setEntities(list.entities);
          }
        }

        if (selectedId === null) {
          if (!useEditorStore.getState().dragActive) {
            useEditorStore.getState().setComponentsBySelected(null);
          }
          return;
        }

        const inspected = await client.inspect(selectedId);
        if (stopped) {
          return;
        }
        if (!useEditorStore.getState().dragActive) {
          useEditorStore.getState().setComponentsBySelected(inspected);
        }
      } catch {
      } finally {
        refreshInFlight = false;
        if (pendingRefresh !== null && !stopped) {
          const next = pendingRefresh;
          pendingRefresh = null;
          refreshHeavyState(next);
        }
      }
    })();
  };

  /// Refresh the selected rig's animation state + clips for the TimelinePanel. Driven by
  /// the animationVersion gate (a play/seek/pause bumped the player) or a selection change.
  /// `get-animation-state` rejects when the entity is not an animation player — that is the
  /// "not rigged / nothing playing yet" case, so clear the slice silently (no toast).
  const refreshAnimation = (selectedId: string | null): void => {
    if (animationInFlight) {
      return;
    }
    if (selectedId === null) {
      useEditorStore.getState().setAnimationState(null, []);
      return;
    }
    animationInFlight = true;
    void (async () => {
      try {
        const [state, clips] = await Promise.all([
          client.getAnimationState(selectedId).catch(() => null),
          client.listClips(selectedId).catch(() => ({ clips: [] })),
        ]);
        if (stopped) {
          return;
        }
        useEditorStore.getState().setAnimationState(state, clips.clips);
      } catch {
      } finally {
        animationInFlight = false;
      }
    })();
  };

  /// The perf-telemetry lane (decoupled from the cheap state tick). Alarms are drained
  /// every tick so the badge stays live with the panel closed; the heavier frame-history
  /// and per-pass reads only run while the Stats dashboard is open.
  const pollMetrics = async (): Promise<void> => {
    if (stopped || metricsInFlight || !readyForSync()) {
      return;
    }
    metricsInFlight = true;
    try {
      const drained = await client.drainAlarms(alarmSince);
      if (stopped) {
        return;
      }
      if (drained.events.length > 0) {
        useEditorStore.getState().appendAlarmEvents(drained.events);
        routeAlarmToasts(drained.events, performance.now());
      }
      alarmSince = Math.max(alarmSince, drained.highWaterSeq);

      // Contained script errors pause play engine-side; surface the traceback here.
      // Drained while play is active (a pause keeps the state visible until stop).
      const playState = useEditorStore.getState().playState;
      if (playState !== "edit") {
        if (lastPlayStateForScripts === "edit") {
          resetScriptErrorToasts(); // fresh session: the engine cleared its ring
        }
        const scriptErrors = await client.drainScriptErrors(scriptErrorSince);
        if (stopped) {
          return;
        }
        if (scriptErrors.events.length > 0) {
          routeScriptErrorToasts(scriptErrors.events);
        }
        scriptErrorSince = Math.max(scriptErrorSince, scriptErrors.highWaterSeq);
      }
      lastPlayStateForScripts = playState;

      const active = await client.listActiveAlarms();
      if (stopped) {
        return;
      }
      useEditorStore.getState().setActiveAlarms(active.alarms);

      // Fetch the shared config once; the target-FPS dropdown refreshes it on write.
      if (useEditorStore.getState().perfConfig === null) {
        const config = await client.getPerfConfig();
        if (stopped) {
          return;
        }
        useEditorStore.getState().setPerfConfig(config);
      }

      if (useEditorStore.getState().rightTools.includes("stats")) {
        const history = await client.frameHistory(FRAME_HISTORY_SAMPLES);
        if (stopped) {
          return;
        }
        appendFrameSamples(history.samples);
        useEditorStore.getState().setFrameHistory(history);
        const stats = useEditorStore.getState().renderStats;
        if (stats && stats.profilerMode !== "off") {
          const passes = await client.passTimings();
          if (stopped) {
            return;
          }
          useEditorStore.getState().setPassTimings(passes);
        }
      }
    } catch {
      // Engine briefly busy; the next tick recovers.
    } finally {
      metricsInFlight = false;
    }
  };

  const watchdog = (): void => {
    if (stopped || watchdogInFlight || !engineMayBeAlive()) {
      return;
    }
    watchdogInFlight = true;
    void client
      .engineAlive()
      .then((alive) => {
        if (!alive && !stopped) {
          useEditorStore.getState().setPhase("error", "Engine process exited.");
        }
      })
      .catch(() => {
        if (!stopped) {
          useEditorStore.getState().setPhase("error", "Engine process exited.");
        }
      })
      .finally(() => {
        watchdogInFlight = false;
      });
  };

  async function tick(): Promise<void> {
    if (stopped || fastInFlight) {
      if (!stopped) {
        schedule();
      }
      return;
    }
    fastInFlight = true;
    try {
      if (!readyForSync()) {
        lastTickAt = 0;
        return;
      }

      const now = performance.now();
      if (lastTickAt !== 0) {
        const interval = now - lastTickAt;
        emaIntervalMs = emaIntervalMs === 0 ? interval : emaIntervalMs * 0.8 + interval * 0.2;
        if (emaIntervalMs > 0) {
          useEditorStore.getState().setPollRateHz(1000 / emaIntervalMs);
        }
      }
      lastTickAt = now;

      const [selection, stats, gizmo] = await Promise.all([
        client.getSelection(),
        client.renderStats(),
        client.getGizmo(),
      ]);
      if (stopped) {
        return;
      }

      const live = useEditorStore.getState();
      if (live.dragActive || live.engineStatus.phase !== "ready") {
        return;
      }

      live.setRenderStats(stats);
      // Reflect the engine's gizmo state (so an external `se set-gizmo` shows up
      // in the Topbar); Topbar clicks set this optimistically, the poll confirms.
      live.setGizmo(gizmo);

      // Mirror play state from the engine on a version change (a shell `se play`
      // shows up here too); Topbar clicks set it optimistically, the poll confirms.
      // The stop-driven sceneVersion bump rides the refreshHeavyState path below,
      // snapping the tree/inspector back to the authored scene.
      if (selection.playVersion !== knownPlayVersion) {
        live.setPlayState(selection.playState as PlayState);
      }

      const nextSelectedId = selection.entity ? selection.entity.id : null;
      const previousSceneVersion = knownSceneVersion;
      const sceneChanged = selection.sceneVersion !== knownSceneVersion;
      const selectionChanged =
        selection.selectionVersion !== knownSelectionVersion || nextSelectedId !== knownSelectedId;

      live.setSelectionVersion(selection.selectionVersion);
      live.setSceneVersion(selection.sceneVersion);
      if (selectionChanged) {
        live.setSelectedId(nextSelectedId);
      }

      refreshHeavyState({
        selectedId: nextSelectedId,
        sceneChanged,
        selectionChanged,
        sceneVersion: selection.sceneVersion,
        previousSceneVersion,
      });

      // Animation gate, parallel to the playVersion gate: refetch the selected rig's
      // playhead/clips when the engine bumps animationVersion (a play/seek/pause, here
      // or via the `se` CLI) or when the selection changes to a different entity. The
      // returned `time` drives the TimelinePanel playhead (canvas, never React state).
      if (selection.animationVersion !== knownAnimationVersion || selectionChanged) {
        refreshAnimation(nextSelectedId);
      }

      knownSceneVersion = selection.sceneVersion;
      knownSelectionVersion = selection.selectionVersion;
      knownSelectedId = nextSelectedId;
      knownPlayVersion = selection.playVersion;
      knownAnimationVersion = selection.animationVersion;
    } catch {
    } finally {
      fastInFlight = false;
      schedule();
    }
  }

  // The metrics lane: a base tick that fetches only when the configured interval has
  // elapsed and the lane is not paused — so the refresh-rate and pause controls take
  // effect within one base tick.
  const metricsTick = (): void => {
    if (stopped) {
      return;
    }
    const state = useEditorStore.getState();
    const now = performance.now();
    if (!state.metricsPaused && now - lastMetricsFetchAt >= state.metricsRefreshMs) {
      lastMetricsFetchAt = now;
      void pollMetrics().finally(() => {
        if (!stopped) {
          metricsTimer = setTimeout(metricsTick, METRICS_BASE_TICK_MS);
        }
      });
      return;
    }
    metricsTimer = setTimeout(metricsTick, METRICS_BASE_TICK_MS);
  };

  schedule();
  watchdogTimer = setInterval(watchdog, WATCHDOG_INTERVAL_MS);
  metricsTimer = setTimeout(metricsTick, METRICS_BASE_TICK_MS);

  return () => {
    stopped = true;
    if (fastTimer !== null) {
      clearTimeout(fastTimer);
      fastTimer = null;
    }
    if (watchdogTimer !== null) {
      clearInterval(watchdogTimer);
      watchdogTimer = null;
    }
    if (metricsTimer !== null) {
      clearTimeout(metricsTimer);
      metricsTimer = null;
    }
  };
}

/// Client-side thumbnail cache: assetId → { blob URL, the px size it was fetched
/// at }. Kept at module scope (NOT in Zustand) because it holds blob URLs that
/// must survive re-renders without provoking store churn, and a single fetch must
/// be shared by all tiles/pickers/viewers asking for the same asset.
///
/// A get-thumbnail call is a GPU→CPU readback + PNG encode (off the present path),
/// so we fetch lazily on first view, dedupe concurrent requests, and re-use a
/// cached URL whenever the cached image is at least as large as the requested
/// size (a 128 grid tile and a 16-from-64 combo swatch both read the 64/128 blob).
interface ThumbnailCacheEntry {
  url: string;
  size: number;
}

const thumbnailCache = new Map<string, ThumbnailCacheEntry>();
/// In-flight fetches keyed by assetId, so N tiles mounting at once issue ONE
/// get-thumbnail per asset (the rest await the same promise).
const thumbnailInflight = new Map<string, Promise<string>>();

function base64ToBlob(b64: string, mime = "image/png"): Blob {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) {
    bytes[i] = bin.charCodeAt(i);
  }
  return new Blob([bytes], { type: mime });
}

export function getCachedThumbnailUrl(assetId: string, size: number): string | null {
  const cached = thumbnailCache.get(assetId);
  return cached && cached.size >= size ? cached.url : null;
}

/// Resolve a blob URL for an asset's thumbnail at (at least) `size` px. Cache hit
/// (same or larger cached size) returns immediately; a miss fetches once over the
/// socket, decodes the base64 PNG, and stores the object URL. A `pending` reply means
/// the engine is generating it on a worker thread (cold cache) — re-request with
/// backoff until it lands (a pure disk-cache hit). Rejects (engine error) to let the
/// caller fall back to a type icon. The in-flight map keeps one retry loop per asset.
export async function getThumbnailUrl(assetId: string, size: number): Promise<string> {
  const cached = getCachedThumbnailUrl(assetId, size);
  if (cached) {
    return cached;
  }
  const inflight = thumbnailInflight.get(assetId);
  if (inflight) {
    return inflight;
  }
  const promise = (async (): Promise<string> => {
    let delayMs = 60;
    for (;;) {
      const thumb = await client.getThumbnail(assetId, size);
      if (!thumb.pending) {
        const url = URL.createObjectURL(base64ToBlob(thumb.base64));
        const prev = thumbnailCache.get(assetId);
        if (prev) {
          URL.revokeObjectURL(prev.url);
        }
        thumbnailCache.set(assetId, { url, size });
        return url;
      }
      await new Promise((resolve) => setTimeout(resolve, delayMs));
      delayMs = Math.min(delayMs * 2, 1000); // exponential backoff, capped at 1s
    }
  })();
  thumbnailInflight.set(assetId, promise);
  try {
    return await promise;
  } finally {
    thumbnailInflight.delete(assetId);
  }
}

/// Revoke every cached blob URL and clear the cache. Called on a scene/project
/// load (the catalog changed, so cached images are stale) — see the reconcile
/// poll's scene-reset branch.
export function invalidateThumbnails(): void {
  for (const entry of thumbnailCache.values()) {
    URL.revokeObjectURL(entry.url);
  }
  thumbnailCache.clear();
}
