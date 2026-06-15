module;

// The editor's shared types + public surface. entt/glm are header-heavy, so this
// partition uses classic includes (no `import std`), like the rest of the editor.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

export module Saffron.SceneEdit:Context;

import Saffron.Core;
import Saffron.Signal;
import Saffron.Scene;

export namespace sa
{
    // The viewport's own fly-camera (the scene-view eye, distinct from any ECS
    // CameraComponent / game camera). Hold RMB over the viewport to look + WASD to move,
    // Space up / Shift down. yaw/pitch in degrees; at yaw 0 the camera looks down -Z.
    struct SceneEditCamera
    {
        glm::vec3 position{ 3.0f, 2.5f, 4.0f };
        f32 yaw = -37.0f;
        f32 pitch = -29.0f;
        f32 fov = 45.0f;
        f32 nearPlane = 0.1f;
        f32 farPlane = 100.0f;
        f32 moveSpeed = 6.0f;           // units/second
        f32 lookSpeed = 0.12f;          // degrees/pixel
        glm::vec2 lookPending{ 0.0f };  // undelivered look pixels, drained exponentially per frame
        bool controlling = false;       // latched while RMB is held (so a drag can leave the rect)
    };

    // Backend-neutral per-frame fly-cam input. The host (which owns SDL) fills this so
    // SceneEdit stays SDL-free. lookDelta is summed relative-mouse motion (pixels) for the
    // frame; the bools are the move-key state during the RMB hold.
    struct SceneEditCameraInput
    {
        bool active = false;  // RMB-fly engaged (keyboard grabbed)
        glm::vec2 lookDelta{ 0.0f };
        bool forward = false;
        bool back = false;
        bool left = false;
        bool right = false;
        bool up = false;    // Space
        bool down = false;  // LShift
    };

    // Backend-neutral gizmo op + reference space, shared by the control TU and the native overlay.
    enum class GizmoOp
    {
        Translate,
        Rotate,
        Scale
    };
    enum class GizmoSpace
    {
        World,
        Local
    };

    auto gizmoOpName(GizmoOp op) -> const char*;  // "translate"|"rotate"|"scale"
    auto gizmoOpFromName(const std::string& name) -> GizmoOp;
    auto gizmoSpaceName(GizmoSpace space) -> const char*;  // "world"|"local"
    auto gizmoSpaceFromName(const std::string& name) -> GizmoSpace;

    // The engine-rendered (overlay) gizmo. mode/space are driven FROM the backend-neutral
    // GizmoOp/GizmoSpace (the single source) — mapped each frame. The remaining fields are
    // the overlay's own hover/drag interaction state.
    enum class NativeGizmoMode
    {
        Translate,
        Rotate,
        Scale
    };
    enum class NativeGizmoSpace
    {
        World,
        Local
    };
    enum class NativeGizmoHandle
    {
        None,
        X,
        Y,
        Z,
        XY,
        YZ,
        XZ,
        Screen,
        Uniform
    };

    struct NativeGizmoState
    {
        NativeGizmoMode mode = NativeGizmoMode::Translate;
        NativeGizmoSpace space = NativeGizmoSpace::World;
        NativeGizmoHandle hovered = NativeGizmoHandle::None;
        NativeGizmoHandle active = NativeGizmoHandle::None;
        bool dragging = false;
        glm::vec2 startMouse{ 0.0f };
        glm::vec2 dragTarget{ 0.0f };        // latest raw pointer sample (viewport pixels)
        glm::vec2 dragSmoothed{ 0.0f };      // per-frame smoothed pointer the drag math consumes
        bool dragPending = false;            // command-driven drag: smooth + apply each frame
        glm::vec3 startTranslation{ 0.0f };  // world translation at drag begin
        glm::vec3 startRotation{ 0.0f };     // world rotation (Euler) at drag begin
        glm::vec3 startScale{ 1.0f };        // local scale (scale never rebases)
        glm::mat4 startParentWorld{ 1.0f };  // frozen parent world for the whole drag
        Entity target{ entt::null };
        // Direct-child worlds frozen at drag begin (filled only with preserveChildren);
        // each applied drag frame rebases these locals so the children hold their pose.
        std::vector<std::pair<entt::entity, glm::mat4>> startChildWorlds;
    };

    // A pending smoothed material edit: per-field targets the per-frame stepper
    // converges the entity's MaterialComponent toward (`set-material smooth:1`).
    // Absent fields are untouched; repeated smooth sends merge into the entry.
    struct MaterialSmoothTarget
    {
        Entity entity{ entt::null };
        std::optional<glm::vec4> baseColor;
        std::optional<f32> metallic;
        std::optional<f32> roughness;
        std::optional<glm::vec3> emissive;
        std::optional<f32> emissiveStrength;
    };

    // The TransformComponent counterpart (`set-transform smooth:1`): Inspector
    // scrubs converge like gizmo drags instead of stepping at the send rate.
    struct TransformSmoothTarget
    {
        Entity entity{ entt::null };
        std::optional<glm::vec3> translation;
        std::optional<glm::vec3> rotation;
        std::optional<glm::vec3> scale;
    };

    // Editor play mode: Edit -> Playing <-> Paused -> Edit. Session policy — lives on
    // SceneEditContext, never on Scene, and never serializes into the project.
    enum class PlayState
    {
        Edit,
        Playing,
        Paused
    };

    auto playStateName(PlayState state) -> const char*;  // "edit"|"playing"|"paused"
    auto playStateFromName(const std::string& name) -> PlayState;

    // One contained script failure, kept in a bounded ring on the context so the
    // editor drains it over a normal scene command (Control never imports the Lua
    // runtime). entityUuid is 0 when the failure has no owning entity.
    struct ScriptError
    {
        i64 seq = 0;
        u64 entityUuid = 0;
        std::string script;
        std::string message;
        i64 tick = 0;  // the play tick the error fired on
    };

    inline constexpr std::size_t ScriptErrorRingCap = 256;

    // One `sa.log(...)` line, kept in a bounded ring on the context and drained over a normal scene
    // command (the same path as ScriptError; Control never imports the Lua runtime). entityUuid is the
    // logging entity, or 0 when logged outside an entity handler. epochMs is a wall-clock stamp for
    // display only — never serialized, never part of determinism.
    struct ScriptLog
    {
        i64 seq = 0;
        u64 entityUuid = 0;
        std::string message;
        i64 epochMs = 0;
        i64 tick = 0;
    };

    inline constexpr std::size_t ScriptLogRingCap = 1024;

    inline constexpr f32 PlayFixedStep = 1.0f / 60.0f;  // the deterministic `step` tick
    inline constexpr f32 PlayMaxDelta = 1.0f / 3.0f;    // dt clamp so a hitch never spikes the simulation

    // Line-skeleton viewport overlay for the selected rig: bone segments + joint dots,
    // with optional per-joint RGB axes. Opt-in (show defaults false); drawn in Edit and Play.
    struct SkeletonOverlayOptions
    {
        bool show = false;        // master toggle (set-skeleton-overlay)
        bool axes = false;        // per-joint RGB axis lines
        f32 jointSize = 4.0f;     // joint-dot radius in pixels (screen-constant at any zoom)
        i32 highlightJoint = -1;  // get-asset-model node index of the tinted joint while previewing, -1 = none
    };

    // Viewport debug overlays (set-debug-overlays), drawn as world-space lines in the editor
    // overlay pass. Opt-in, Edit-only, saved into project.json via debugOverlaysToJson.
    struct DebugOverlayOptions
    {
        bool bounds = false;        // per-entity world AABB (the pick volume for static meshes)
        bool sceneAabb = false;     // the whole-scene AABB the shadow/DDGI fit uses
        bool lightVolumes = false;  // point-light range spheres + spot cones
        bool grid = false;          // infinite analytic ground grid (a render-graph pass)
        bool colliders = false;     // physics collision shapes (box/sphere/capsule wireframes), Edit + Play
    };

    // The editor's mutable state: the scene being edited, the component registry
    // that drives every panel, and the current selection (broadcast as a signal).
    struct SceneEditContext
    {
        Scene scene;
        ComponentRegistry registry;
        Entity selected{ entt::null };
        SubscriberList<Entity> onSelectionChanged;
        std::string scenePath;
        bool projectLoaded = false;
        std::string projectRoot;
        std::string projectPath;
        std::string projectName;
        std::string projectDisplayName;
        SceneEditCamera camera;
        u64 sceneVersion = 0;      // bumped by add/copy/destroy-entity + load (control-plane diff poll)
        u64 selectionVersion = 0;  // bumped on every selection change

        GizmoOp gizmoOp = GizmoOp::Translate;       // W/E/R cycle translate/rotate/scale
        GizmoSpace gizmoSpace = GizmoSpace::World;  // gizmo reference space (world/local)
        bool preserveChildren = false;              // transform a parent without moving its children
                                                    // (their locals rebase to hold the world pose)
        NativeGizmoState nativeGizmo;               // overlay-gizmo hover/drag state (mode/space synced from above)
        SkeletonOverlayOptions skeletonOverlay;     // line-skeleton viewport overlay for the selected rig
        DebugOverlayOptions debugOverlays;          // bounds / scene-AABB / light-volume viewport overlays
        std::vector<MaterialSmoothTarget> materialSmoothing;    // pending smoothed material edits, one per entity
        std::vector<TransformSmoothTarget> transformSmoothing;  // pending smoothed transform edits, one per entity
        SceneEditCameraInput flyInput;                          // latest fly-input command state; lookDelta accumulates
                                                                // until the host drains it each frame

        PlayState playState = PlayState::Edit;
        std::optional<Scene> playScene;                // the throwaway play duplicate; nullopt in Edit
        u64 playVersion = 0;                           // bumped on every play transition (reconcile-poll stamp)
        u64 animationVersion = 0;                      // bumped by the animation commands (play/pause/seek/loop)
        i32 stepFrames = 0;                            // pending single-step ticks, granted only while Paused
        bool hadPrimaryCamera = false;                 // captured at enterPlay; false drives the editor warning
        SubscriberList<PlayState> onPlayStateChanged;  // the physics/scripting lifecycle seam

        // The simulation seam: tickPlay invokes this with the active (play) scene and
        // the clamped dt. The Host points it at the script runtime; std-only here.
        std::function<void(Scene&, f32)> simTick;
        i64 playTick = 0;                       // ticks run since enterPlay (error timestamps)
        i32 scriptInstanceCount = 0;            // live script instances; set by the Host wiring
        std::vector<ScriptError> scriptErrors;  // bounded ring, oldest dropped at ScriptErrorRingCap
        i64 scriptErrorSeq = 0;                 // last assigned ScriptError.seq (drain high-water)
        std::vector<ScriptLog> scriptLogs;      // bounded ring, oldest dropped at ScriptLogRingCap
        i64 scriptLogSeq = 0;                   // last assigned ScriptLog.seq (drain high-water)
        ScriptInputState scriptInput;  // raw gameplay input for Lua (held keys + mouse); edges derived per tick

        // The asset preview (the asset editor): an isolated scene holding only the previewed model + its
        // furnishing, entered/left over the control plane. Routed through activeScene exactly like the
        // play duplicate, so render/skinning/animation/commands retarget to it for free. Stays in
        // PlayState::Edit (mutually exclusive with play), and never leaks into project.json because
        // save-project serializes `scene` explicitly.
        std::optional<Scene> previewScene;        // the isolated preview scene; nullopt when not previewing
        Uuid previewAsset{ 0 };                   // the model container being previewed (0 = none)
        Entity previewRootEntity{ entt::null };   // the spawned model root entity in previewScene
        std::vector<Uuid> previewBoneByNode;      // node index -> spawned joint entity uuid (empty for a static model)
        Entity savedSelection{ entt::null };      // authored-scene selection stashed across the preview
        SceneEditCamera savedCamera;              // fly-cam stashed on enter, restored on exit (byte-identity)
        SkeletonOverlayOptions savedOverlay;      // overlay prefs stashed on enter (preview forces it on)
        bool previewShowFloor = true;             // the preview floor slab toggle (set-asset-preview-options)
        Entity previewFloorEntity{ entt::null };  // the spawned floor slab in previewScene (for the toggle)
        // True when the asset preview is the active view (set-active-view AssetPreview). The preview scene
        // can stay alive while this is false (the asset tab is open but the scene view is active), so a
        // view switch is instant — no re-spawn; the orbit camera is parked in parkedPreviewCamera and
        // restored when the preview view becomes active again. activeScene routes to the preview only while
        // this holds, and previewing() reads it, so the authored scene is fully editable when it is false.
        bool previewActiveView = false;
        SceneEditCamera parkedPreviewCamera;  // the preview orbit fly-cam, parked while the scene view is active
    };

    // Append to the bounded script-error ring, stamping seq + the current play tick.
    void pushScriptError(SceneEditContext& ctx, u64 entityUuid, std::string script, std::string message);

    // Append a `sa.log` line to the bounded script-log ring, stamping seq + a wall-clock ms + the tick.
    void pushScriptLog(SceneEditContext& ctx, u64 entityUuid, std::string message);

    // The scene every consumer addresses: the asset preview while it is the active view, the play
    // duplicate while playing/paused, the authored scene in Edit. Preview takes precedence (it is entered
    // only from Edit). Nothing else may branch on playState/previewScene to pick a scene.
    inline auto activeScene(SceneEditContext& ctx) -> Scene&
    {
        if (ctx.previewScene && ctx.previewActiveView)
        {
            return *ctx.previewScene;
        }
        if (ctx.playState == PlayState::Edit)
        {
            return ctx.scene;
        }
        return *ctx.playScene;
    }

    // True while the asset preview is the ACTIVE view (entered and selected via set-active-view).
    // Commands that mutate the authored scene or project must refuse while this holds — activeScene routes
    // to the preview, so they would otherwise edit or render the wrong scene (and could leak preview state
    // into a save). With the scene view active this reads false: the authored scene is active again and
    // fully editable, even if a preview scene is kept alive in the background.
    inline auto previewing(const SceneEditContext& ctx) -> bool
    {
        return ctx.previewScene.has_value() && ctx.previewActiveView;
    }

    // The payload dragged from an asset tile onto a component picker field.
    struct AssetDragPayload
    {
        u64 id = 0;
        AssetType type = AssetType::Mesh;
    };

    void setSelection(SceneEditContext& ctx, Entity entity);

    // Play-mode transitions. Each one validates the current state, bumps playVersion, and
    // publishes onPlayStateChanged. enterPlay duplicates the authored scene through the
    // JSON serde ("create this scene again" — the duplicate is what a save/load would
    // produce); stopPlay discards the duplicate, so there is no restore step to get wrong.
    auto enterPlay(SceneEditContext& ctx) -> Result<void>;   // Edit -> Playing
    auto pausePlay(SceneEditContext& ctx) -> Result<void>;   // Playing -> Paused
    auto resumePlay(SceneEditContext& ctx) -> Result<void>;  // Paused -> Playing
    // Grants `frames` single-step ticks; Paused only. Does not change state.
    auto stepPlay(SceneEditContext& ctx, i32 frames) -> Result<void>;
    // Playing|Paused -> Edit; an idempotent no-op in Edit.
    auto stopPlay(SceneEditContext& ctx) -> Result<void>;
    // The host onUpdate driver: gates the runtime tick on playState and pending steps.
    // The tick body is the simulation seam (physics/scripting/animation); empty in v1.
    void tickPlay(SceneEditContext& ctx, f32 dt);

    // The eye the viewport renders from: the editor fly-camera in Edit; the active scene's
    // primary CameraComponent during play, falling back to the fly-camera (never black)
    // when the scene has none. The host onUi and the pick command share this so a click
    // ray-casts from the same camera the frame was rendered with.
    auto renderCameraView(SceneEditContext& ctx) -> CameraView;

    // Headless play-mode check: state-machine invariants, duplicate fidelity, the
    // pause/step gate, and the discard guarantee. Results land in the log.
    void runPlayModeSelfTest();

    // Concrete built-in component registration — the json serde lambdas live here. The
    // present-only native-viewport host renders no inspector, so the per-component
    // drawInspector is a no-op; the registry exists for its serialize/deserialize.
    void registerBuiltinComponents(ComponentRegistry& reg);

    // Heap-owned so SceneEditContext's heavy destructor (entt/json) is instantiated
    // here, not in the client TU. The editor holds only the pointer.
    auto newSceneEditContext() -> SceneEditContext*;
    void destroySceneEditContext(SceneEditContext* ctx);

    // The editor camera's forward (world space) from its yaw/pitch.
    auto sceneEditCameraForward(const SceneEditCamera& camera) -> glm::vec3;

    // The editor camera as a Scene CameraView (view + projection params), so renderScene
    // and the gizmo draw from the same eye.
    auto sceneEditCameraView(const SceneEditCamera& camera) -> CameraView;

    // The persisted editor view (position/yaw/pitch/fov), saved into project.json so a
    // reopened project shows the same framing. Missing fields keep their current value.
    auto sceneEditCameraToJson(const SceneEditCamera& camera) -> nlohmann::json;
    void sceneEditCameraFromJson(SceneEditCamera& camera, const nlohmann::json& j);

    // The viewport debug overlays (bounds / scene-AABB / light-volumes / grid), saved into
    // project.json so a reopened project restores them. Missing fields keep their current value.
    auto debugOverlaysToJson(const DebugOverlayOptions& opts) -> nlohmann::json;
    void debugOverlaysFromJson(DebugOverlayOptions& opts, const nlohmann::json& j);

    // Fly the editor camera from host-gathered SDL input (active while RMB is held): mouse
    // look + WASD move, Space up / Shift down (world Y). Call from onUpdate with the frame dt.
    void updateSceneEditCamera(SceneEditCamera& camera, const SceneEditCameraInput& input, f32 dt);

    // Native (overlay) gizmo math — pure glm + Scene types, no Rendering. Shared by the
    // SDL event sink (editor app) and the gizmo-pointer control command so both drive one
    // hit-test/drag implementation.

    // A world point projected to the viewport: pixel + NDC + whether it is on-screen.
    struct GizmoProjection
    {
        glm::vec2 pixel{ 0.0f };
        glm::vec2 ndc{ 0.0f };
        bool visible = false;
    };

    // Projects a world point through the camera to viewport pixels (top-left origin) + NDC.
    auto viewportProject(const CameraView& cam, u32 width, u32 height, glm::vec3 world) -> GizmoProjection;
    // Viewport pixel (top-left origin) to clip-space NDC.
    auto pixelToNdc(glm::vec2 p, u32 width, u32 height) -> glm::vec2;
    // The camera's world position from its view matrix.
    auto cameraPosition(const CameraView& cam) -> glm::vec3;
    // Distance from p to segment [a,b], all in pixels.
    auto pointSegmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b) -> f32;
    // The display color for a gizmo handle (axis-tinted; highlighted when hovered/active).
    auto axisColor(NativeGizmoHandle handle, const NativeGizmoState& gizmo) -> glm::vec4;
    // The gizmo's X/Y/Z basis: world identity, or the entity's world-rotated basis in Local space.
    auto gizmoAxes(const glm::quat& worldRotation, NativeGizmoSpace space) -> std::array<glm::vec3, 3>;
    // The world-space axis for a single-axis handle (zero for plane/screen/uniform handles).
    auto handleAxis(NativeGizmoHandle handle, const std::array<glm::vec3, 3>& axes) -> glm::vec3;
    // The projected corners of a two-axis translate plane handle (axes `first`/`second`),
    // shared by the overlay drawing and the hit-test so they always agree.
    auto gizmoPlaneCorners(const CameraView& cam, u32 width, u32 height, glm::vec3 position,
                           const std::array<glm::vec3, 3>& axes, f32 axisLen, u32 first, u32 second)
        -> std::array<GizmoProjection, 4>;
    // An orthonormal basis spanning the plane perpendicular to `n` (the rotation ring
    // plane), NaN-safe for any axis including world up. Shared by the ring drawing and
    // hit-test so both walk identical circles.
    auto ringBasis(glm::vec3 n) -> std::pair<glm::vec3, glm::vec3>;
    // Hit-tests the selected entity's gizmo at `mouse` (viewport pixels) for the active mode/space.
    auto hitNativeGizmo(SceneEditContext& editor, const CameraView& cam, u32 width, u32 height, glm::vec2 mouse)
        -> NativeGizmoHandle;
    // Applies an in-progress gizmo drag, writing the dragged entity's TransformComponent.
    void applyNativeGizmoDrag(SceneEditContext& editor, const CameraView& cam, u32 width, u32 height, glm::vec2 mouse);
    // Advances a command-driven drag each rendered frame: exponentially smooths the pointer
    // toward dragTarget and applies the drag, so ~60Hz control samples render fluidly at
    // the engine's frame rate. No-op unless a gizmo-pointer drag sample is pending.
    void stepNativeGizmoDrag(SceneEditContext& editor, const CameraView& cam, u32 width, u32 height, f32 dt);
    // The smooth entry for an entity, appended if absent (a `smooth:1` edit merges its
    // fields here instead of writing the component).
    auto materialSmoothEntryFor(SceneEditContext& editor, Entity entity) -> MaterialSmoothTarget&;
    auto transformSmoothEntryFor(SceneEditContext& editor, Entity entity) -> TransformSmoothTarget&;
    // Drop an entity's smooth entry — an exact (non-smooth) write always wins.
    void cancelMaterialSmoothing(SceneEditContext& editor, Entity entity);
    void cancelTransformSmoothing(SceneEditContext& editor, Entity entity);
    // Converges every smoothed edit (material + transform) toward its targets each
    // rendered frame (same exponential as the gizmo pointer smoothing), snapping exactly
    // and dropping the entry once converged. Call from onUpdate with the frame dt.
    void stepEditSmoothing(SceneEditContext& editor, f32 dt);
    // Captures the drag-begin state (world translation/rotation, local scale, frozen parent
    // world) — the one snapshot both the SDL and control gizmo-pointer paths share.
    void snapshotNativeGizmoStart(SceneEditContext& editor, Entity target);

    // Mirrors the backend-neutral GizmoOp/GizmoSpace (the single source) onto nativeGizmo.mode/.space.
    void syncNativeGizmo(SceneEditContext& ctx);
}
