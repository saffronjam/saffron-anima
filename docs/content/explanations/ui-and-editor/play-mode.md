+++
title = 'Play mode'
weight = 5
+++

# Play mode

Play mode runs the scene the way the game will. Pressing Play switches the viewport from the editor fly-camera to the scene's own camera and starts a runtime tick; Stop returns to editing with the authored scene exactly as it was. Pause and Step in between let you freeze a frame and advance one tick at a time.

The model is Unreal's play-in-editor adapted to this engine: Play does not mutate the scene you authored. It duplicates it, runs everything against the copy, and throws the copy away on Stop. So "what did play change?" has the same answer every time — nothing. You can move things, retune lights, spawn entities, and none of it survives the Stop.

## Duplicate, don't restore

Entering play serializes the edit scene and deserializes it into a fresh throwaway scene — the copy is *defined* as "create this scene again", so it equals what loading a saved project would produce. Everything that touches the scene while playing — rendering, picking, every control command — routes through one chokepoint, `activeScene`, which hands back the play duplicate while playing and the authored scene otherwise. The edit scene is never writable through that chokepoint during play.

Stop is then just `playScene.reset()`. There is no restore step to get wrong: the authored scene was never aliased, so dropping the duplicate *is* the restore. This is why the discard guarantee is structural rather than a careful undo — a runtime system can mutate anything in the play world and the authored scene cannot feel it.

The duplicate is cheap here. GPU meshes and textures are keyed by uuid on the asset server and shared by both scenes, so duplication copies only component structs, not GPU resources. On a typical scene the Play press costs a fraction of a millisecond.

## Camera handover

In edit mode the viewport renders through the fly-camera. While playing it renders through the scene's primary `CameraComponent` — evaluated every frame, so a camera animated or re-flagged during play is honored live. A scene with no primary camera falls back to the fly-camera and reports `hasPrimaryCamera: false`, which the editor surfaces as a toast ("No primary camera — using the editor camera"). Play never renders a black frame for the lack of a camera; it warns and keeps going.

The fly-camera stays controllable during play, which is what makes the fallback usable and what keeps `get-camera`/`set-camera`/`fly-input` outside the discard — the editor camera is session state, never part of the duplicated scene.

## The state machine

`Edit → Playing ↔ Paused → Edit`. Pause freezes only the runtime tick; rendering, the control plane, and the fly-camera keep running, so a paused frame stays fully inspectable. Step advances exactly one fixed tick (1/60 s) and is accepted only while paused, so single-stepping is deterministic rather than tied to wall-clock frames. A reserved max-delta clamp keeps a hitch from spiking the simulation once physics arrives.

The tick seam exists and is wired, but the engine has no physics, scripting, or animation yet, so in this version the play scene holds still. What play mode buys today is the camera cut, the runtime-vs-authored split, and the lifecycle signal (`onPlayStateChanged`) those future systems hang off without touching the machinery again.

## Live-tune-and-discard

Panels stay interactive during play. The hierarchy, inspector, and environment all address the running scene, so you can tweak a light or drag a value and watch it take effect immediately — and lose it on Stop. That is the deliberate model (Unity's and Unreal's), and the guardrail against losing real work to it is the tint: while playing or paused the editor chrome carries an amber inset ring and the topbar tinges amber. The viewport itself stays untinted — it is the game view.

Two things lock during play. The gizmo is hidden (its overlay is editor chrome, and a transform it wrote would be swallowed by the discard), so the T/R/S controls and W/E/R shortcuts grey out. Save, open, reload, and new-project grey out too: scene swaps would pull state out from under the running duplicate, and saving is blocked to avoid mistaking a play-mode tweak for authored, saved state.

## Driving it

The toolbar's playback group is a context-sensitive Play/Pause button, Stop, and Step. The same commands drive the engine over the control plane (`play`, `pause`, `stop`, `step`, `get-play-state`), so a shell `se play` flips the toolbar within a poll cycle, exactly like the gizmo buttons. The keyboard family is Unity's: Ctrl+P play/stop, Ctrl+Shift+P pause/resume, Ctrl+Alt+P step.

The editor learns the engine's play state through its existing reconcile poll — `get-selection` now carries `playState`/`playVersion`, so propagation costs no extra round-trip. A click writes the store optimistically and fires the command; the poll repairs it on failure and reflects any external change.

## In the code

| What | File | Symbols |
|---|---|---|
| State machine + duplication + tick (engine) | `scene_edit_play.cpp` | `enterPlay`, `pausePlay`, `resumePlay`, `stepPlay`, `stopPlay`, `tickPlay` |
| Play state + chokepoint (engine) | `scene_edit_context.cppm` | `PlayState`, `activeScene`, `renderCameraView` |
| Render + tick wiring (engine) | `engine/source/saffron/host/host.cppm` | `layer.onUi`, `layer.onUpdate` |
| Play commands (engine) | `control_commands_scene.cpp` | `play`, `pause`, `stop`, `step`, `get-play-state` |
| Client wrappers | `editor/src/control/client.ts` | `play`, `pause`, `stop`, `step`, `getPlayState` |
| Store slice + poll apply | `editor/src/state/store.ts` | `playState`, `setPlayState` |
| Toolbar group | `editor/src/panels/Topbar.tsx` | `onPlayPause`, `onStop`, `onStep` |
| Hotkeys | `editor/src/app/useGizmoShortcuts.ts` | the Ctrl+P family |
| Tint + locks | `editor/src/app/Layout.tsx`, `editor/src/app/ProjectMenu.tsx` | `playRing`, the disabled menu items |

## Related

- [Rig editor](../rig-editor/) — Preview, the third mode that routes through `activeScene` the same way Play does
- [Editor camera](../editor-camera/) — the fly-camera play falls back to, and which stays live during play
- [Transform gizmo](../gizmo/) — hidden during play, since the play duplicate would swallow its writes
- [Scene hierarchy](../../scene-and-ecs/scene-hierarchy/) — the uuid identity that lets the duplicate and the authored scene resolve the same entities
- [Scene commands](../../tooling-and-control/scene-commands/) — `play`/`pause`/`stop`/`step`/`get-play-state` over the wire
