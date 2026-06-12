module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <string_view>

module Saffron.SceneEdit;

import Saffron.Core;
import Saffron.Signal;
import Saffron.Scene;

namespace se
{
    namespace
    {
        void publishTransition(SceneEditContext& ctx, PlayState next)
        {
            ctx.playState = next;
            ctx.playVersion += 1;
            ctx.onPlayStateChanged.publish(next);
        }

        auto selectedUuidIn(SceneEditContext& ctx, Scene& scene) -> u64
        {
            if (!valid(scene, ctx.selected) || !hasComponent<IdComponent>(scene, ctx.selected))
            {
                return 0;
            }
            return getComponent<IdComponent>(scene, ctx.selected).id.value;
        }

        // Smooth targets hold raw entt handles, which index one specific registry —
        // a half-converged edit must never keep converging against the other scene.
        void dropSmoothing(SceneEditContext& ctx)
        {
            ctx.materialSmoothing.clear();
            ctx.transformSmoothing.clear();
        }
    }

    auto playStateName(PlayState state) -> const char*
    {
        switch (state)
        {
        case PlayState::Playing:
            return "playing";
        case PlayState::Paused:
            return "paused";
        case PlayState::Edit:
            break;
        }
        return "edit";
    }

    auto playStateFromName(const std::string& name) -> PlayState
    {
        if (name == "playing")
        {
            return PlayState::Playing;
        }
        if (name == "paused")
        {
            return PlayState::Paused;
        }
        return PlayState::Edit;
    }

    auto enterPlay(SceneEditContext& ctx) -> Result<void>
    {
        if (ctx.playState != PlayState::Edit)
        {
            return Err("already in play mode — stop first");
        }

        const auto start = std::chrono::steady_clock::now();
        nlohmann::json snap = sceneToJson(ctx.registry, ctx.scene);
        Scene play;
        play.catalog = ctx.scene.catalog;
        auto loaded = sceneFromJson(ctx.registry, play, snap);
        if (!loaded)
        {
            return Err(std::format("play duplicate failed: {}", loaded.error()));
        }

        ctx.hadPrimaryCamera = primaryCamera(play).valid;
        const u64 selectedUuid = selectedUuidIn(ctx, ctx.scene);
        dropSmoothing(ctx);
        ctx.playTick = 0;
        ctx.scriptErrors.clear();  // each session drains fresh; seq stays monotonic for cursors
        ctx.playScene.emplace(std::move(play));
        publishTransition(ctx, PlayState::Playing);
        // Re-resolve the selection into the duplicate by uuid: the old handle indexes the
        // edit registry and could alias an unrelated play entity.
        setSelection(ctx, findEntityByUuid(*ctx.playScene, selectedUuid));

        const f32 ms = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - start).count();
        logInfo(std::format("enterPlay: duplicated the scene in {:.2f} ms", ms));
        return {};
    }

    auto pausePlay(SceneEditContext& ctx) -> Result<void>
    {
        if (ctx.playState != PlayState::Playing)
        {
            return Err("pause requires play mode");
        }
        publishTransition(ctx, PlayState::Paused);
        return {};
    }

    auto resumePlay(SceneEditContext& ctx) -> Result<void>
    {
        if (ctx.playState != PlayState::Paused)
        {
            return Err("resume requires pause");
        }
        ctx.stepFrames = 0;
        publishTransition(ctx, PlayState::Playing);
        return {};
    }

    auto stepPlay(SceneEditContext& ctx, i32 frames) -> Result<void>
    {
        if (ctx.playState != PlayState::Paused)
        {
            return Err("step requires pause");
        }
        if (frames < 1)
        {
            return Err("step frames must be >= 1");
        }
        ctx.stepFrames += frames;
        return {};
    }

    auto stopPlay(SceneEditContext& ctx) -> Result<void>
    {
        if (ctx.playState == PlayState::Edit)
        {
            return {};
        }

        const u64 selectedUuid = selectedUuidIn(ctx, *ctx.playScene);
        dropSmoothing(ctx);
        ctx.playScene.reset();
        ctx.stepFrames = 0;
        ctx.scriptInputKeys.clear();
        // The discard is the restore: the edit scene was never writable through
        // activeScene during play. The sceneVersion bump makes the editor's heavy
        // reconcile re-fetch the authored entity list.
        ctx.sceneVersion += 1;
        publishTransition(ctx, PlayState::Edit);
        // Back by uuid; a runtime-spawned selection has no authored twin and clears.
        setSelection(ctx, findEntityByUuid(ctx.scene, selectedUuid));
        return {};
    }

    auto renderCameraView(SceneEditContext& ctx) -> CameraView
    {
        CameraView cam = sceneEditCameraView(ctx.camera);
        if (ctx.playState == PlayState::Edit)
        {
            return cam;
        }
        CameraView game = primaryCamera(activeScene(ctx));
        if (game.valid)
        {
            return game;
        }
        return cam;
    }

    void tickPlay(SceneEditContext& ctx, f32 dt)
    {
        if (ctx.playState == PlayState::Edit)
        {
            return;
        }
        const bool run = ctx.playState == PlayState::Playing || ctx.stepFrames > 0;
        if (!run)
        {
            return;
        }
        if (ctx.stepFrames > 0)
        {
            ctx.stepFrames -= 1;
            dt = PlayFixedStep;
        }
        dt = std::min(dt, PlayMaxDelta);
        ctx.playTick += 1;
        // The simulation seam: physics, scripting, and animation advance *ctx.playScene
        // here. The Host points simTick at the script runtime.
        if (ctx.simTick)
        {
            ctx.simTick(activeScene(ctx), dt);
        }
    }

    void pushScriptError(SceneEditContext& ctx, u64 entityUuid, std::string script, std::string message)
    {
        ctx.scriptErrorSeq += 1;
        if (ctx.scriptErrors.size() >= ScriptErrorRingCap)
        {
            ctx.scriptErrors.erase(ctx.scriptErrors.begin());
        }
        ctx.scriptErrors.push_back(
            ScriptError{ ctx.scriptErrorSeq, entityUuid, std::move(script), std::move(message), ctx.playTick });
    }

    void runPlayModeSelfTest()
    {
        u32 failures = 0;
        auto expect = [&failures](bool condition, std::string_view what)
        {
            if (!condition)
            {
                failures = failures + 1;
                logError(std::format("play-mode self-test failed: {}", what));
            }
        };

        SceneEditContext* ctx = newSceneEditContext();
        registerBuiltinComponents(ctx->registry);

        Entity cube = createEntity(ctx->scene, "Cube");
        const glm::vec3 authored{ 1.0f, 2.0f, 3.0f };
        getComponent<TransformComponent>(ctx->scene, cube).translation = authored;
        const u64 cubeUuid = getComponent<IdComponent>(ctx->scene, cube).id.value;
        setSelection(*ctx, cube);

        // A distinctive game-camera fov, so renderCameraView's source is unambiguous: the
        // fly-camera reports ctx.camera.fov, the scene's primary camera reports this.
        Entity editCam{ entt::null };
        forEach<CameraComponent>(ctx->scene, [&](Entity e, CameraComponent&) { editCam = e; });
        getComponent<CameraComponent>(ctx->scene, editCam).fov = 12.0f;
        expect(renderCameraView(*ctx).fov == ctx->camera.fov, "edit renders through the fly-camera");

        u32 editCount = 0;
        forEach<IdComponent>(ctx->scene, [&](Entity, IdComponent&) { editCount += 1; });

        expect(!pausePlay(*ctx).has_value(), "pause in edit rejects");
        expect(!resumePlay(*ctx).has_value(), "resume in edit rejects");
        expect(!stepPlay(*ctx, 1).has_value(), "step in edit rejects");
        expect(stopPlay(*ctx).has_value(), "stop in edit is an idempotent success");

        const u64 versionBefore = ctx->playVersion;
        expect(enterPlay(*ctx).has_value(), "enterPlay from edit succeeds");
        expect(ctx->playState == PlayState::Playing, "enterPlay lands in playing");
        expect(ctx->playVersion > versionBefore, "enterPlay bumps playVersion");
        expect(!enterPlay(*ctx).has_value(), "enterPlay while playing rejects");
        expect(ctx->hadPrimaryCamera, "the seeded camera reads as primary");

        u32 playCount = 0;
        forEach<IdComponent>(*ctx->playScene, [&](Entity, IdComponent&) { playCount += 1; });
        expect(playCount == editCount, "duplicate carries the same entity count");
        Entity playCube = findEntityByUuid(*ctx->playScene, cubeUuid);
        expect(valid(*ctx->playScene, playCube), "cube uuid resolves in the duplicate");
        expect(getComponent<TransformComponent>(*ctx->playScene, playCube).translation == authored,
               "cube transform carried into play");
        expect(selectedUuidIn(*ctx, *ctx->playScene) == cubeUuid, "selection re-resolves to the play twin");
        expect(&activeScene(*ctx) == &*ctx->playScene, "activeScene routes to the duplicate");

        expect(renderCameraView(*ctx).fov == 12.0f, "play cuts to the scene's primary camera");
        Entity playCam{ entt::null };
        forEach<CameraComponent>(*ctx->playScene, [&](Entity e, CameraComponent&) { playCam = e; });
        getComponent<CameraComponent>(*ctx->playScene, playCam).primary = false;
        expect(renderCameraView(*ctx).fov == ctx->camera.fov,
               "play without a primary camera falls back to the fly-camera");
        getComponent<CameraComponent>(*ctx->playScene, playCam).primary = true;

        getComponent<TransformComponent>(*ctx->playScene, playCube).translation = glm::vec3(9.0f, 9.0f, 9.0f);
        createEntity(*ctx->playScene, "Runtime");

        expect(!stepPlay(*ctx, 1).has_value(), "step while playing rejects");
        expect(pausePlay(*ctx).has_value(), "pause from playing succeeds");
        expect(!pausePlay(*ctx).has_value(), "pause while paused rejects");
        expect(stepPlay(*ctx, 2).has_value(), "step while paused succeeds");
        tickPlay(*ctx, 1.0f);
        tickPlay(*ctx, 1.0f);
        expect(ctx->stepFrames == 0, "two ticks consume the two stepped frames");
        tickPlay(*ctx, 1.0f);
        expect(ctx->stepFrames == 0, "a paused tick without pending steps stays inert");
        expect(resumePlay(*ctx).has_value(), "resume from paused succeeds");
        expect(!resumePlay(*ctx).has_value(), "resume while playing rejects");

        const u64 sceneVersionBefore = ctx->sceneVersion;
        expect(stopPlay(*ctx).has_value(), "stop from playing succeeds");
        expect(ctx->playState == PlayState::Edit, "stop lands in edit");
        expect(!ctx->playScene.has_value(), "stop drops the duplicate");
        expect(ctx->sceneVersion > sceneVersionBefore, "stop bumps sceneVersion");
        expect(getComponent<TransformComponent>(ctx->scene, cube).translation == authored,
               "authored transform survives play edits untouched");
        u32 afterCount = 0;
        forEach<IdComponent>(ctx->scene, [&](Entity, IdComponent&) { afterCount += 1; });
        expect(afterCount == editCount, "a runtime-spawned entity does not survive stop");
        expect(ctx->selected.handle == cube.handle, "selection restores to the authored entity");

        expect(enterPlay(*ctx).has_value(), "a second enterPlay succeeds");
        setSelection(*ctx, createEntity(*ctx->playScene, "Runtime"));
        expect(stopPlay(*ctx).has_value(), "a second stop succeeds");
        expect(ctx->selected.handle == entt::null, "a runtime-spawned selection clears on stop");

        // The preview accessor: a previewScene takes over activeScene while playState stays Edit, and
        // clearing it returns to the authored scene (mirrors the control-plane enter/exit-rig-preview).
        expect(!previewing(*ctx), "no preview after stop");
        ctx->previewScene.emplace();
        createEntity(*ctx->previewScene, "PreviewRig");
        expect(previewing(*ctx), "a set previewScene reads as previewing");
        expect(ctx->playState == PlayState::Edit, "preview stays in Edit");
        expect(&activeScene(*ctx) == &*ctx->previewScene, "activeScene routes to the preview");
        ctx->previewScene.reset();
        expect(!previewing(*ctx), "clearing previewScene leaves preview");
        expect(&activeScene(*ctx) == &ctx->scene, "activeScene returns to the authored scene");

        destroySceneEditContext(ctx);
        if (failures == 0)
        {
            logInfo("play-mode self-test passed");
        }
    }
}
