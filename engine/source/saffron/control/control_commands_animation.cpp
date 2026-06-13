module;

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <vector>

module Saffron.Control;

import Saffron.Core;
import Saffron.Json;
import Saffron.Rendering;
import Saffron.Scene;
import Saffron.SceneEdit;
import Saffron.Assets;

namespace se
{
    namespace
    {
        using json = nlohmann::json;

        // Resolve an AssetSelector (id-or-name) to an animation catalog entry.
        auto resolveClip(EngineContext& ctx, const AssetSelector& clip) -> Result<const AssetEntry*>
        {
            const json& sel = clip.value;
            std::string name;
            if (sel.is_string())
            {
                name = sel.get<std::string>();
            }
            u64 byId = 0;
            if (sel.is_number_unsigned())
            {
                byId = sel.get<u64>();
            }
            else if (sel.is_number_integer())
            {
                const i64 v = sel.get<i64>();
                if (v >= 0)
                {
                    byId = static_cast<u64>(v);
                }
            }
            else
            {
                byId = std::strtoull(name.c_str(), nullptr, 10);
            }
            for (const AssetEntry& entry : ctx.assets.catalog.entries)
            {
                if (entry.type == AssetType::Animation && (entry.id.value == byId || entry.name == name))
                {
                    return &entry;
                }
            }
            return Err(std::format("no animation clip '{}'", name));
        }

        // Resolve an AssetSelector (id-or-name) to its owning `.smodel` container id: the model's own id
        // for a model, the container for a sub-asset, 0 for a standalone asset.
        auto resolveContainer(EngineContext& ctx, const AssetSelector& asset) -> Result<Uuid>
        {
            const json& sel = asset.value;
            std::string name;
            if (sel.is_string())
            {
                name = sel.get<std::string>();
            }
            u64 byId = 0;
            if (sel.is_number_unsigned())
            {
                byId = sel.get<u64>();
            }
            else if (sel.is_number_integer())
            {
                const i64 v = sel.get<i64>();
                if (v >= 0)
                {
                    byId = static_cast<u64>(v);
                }
            }
            else
            {
                byId = std::strtoull(name.c_str(), nullptr, 10);
            }
            for (const AssetEntry& entry : ctx.assets.catalog.entries)
            {
                if (entry.id.value == byId || entry.name == name)
                {
                    if (entry.type == AssetType::Model)
                    {
                        return entry.id;
                    }
                    return entry.container;
                }
            }
            return Err(std::format("no asset '{}'", name));
        }

        auto wrapName(AnimationPlayerComponent::Wrap wrap) -> std::string
        {
            if (wrap == AnimationPlayerComponent::Wrap::Once)
            {
                return "once";
            }
            if (wrap == AnimationPlayerComponent::Wrap::PingPong)
            {
                return "pingpong";
            }
            return "loop";
        }

        auto wrapFromName(const std::string& name) -> AnimationPlayerComponent::Wrap
        {
            if (name == "once")
            {
                return AnimationPlayerComponent::Wrap::Once;
            }
            if (name == "pingpong")
            {
                return AnimationPlayerComponent::Wrap::PingPong;
            }
            return AnimationPlayerComponent::Wrap::Loop;
        }

        // Resolve the entity and fetch its player, attaching a default one if absent.
        auto playerOf(EngineContext& ctx, const EntitySelector& selector) -> Result<AnimationPlayerComponent*>
        {
            auto entity = resolveEntity(ctx, json{ { "entity", selector.value } });
            if (!entity)
            {
                return Err(entity.error());
            }
            Scene& scene = activeScene(ctx.sceneEdit);
            // The editor selects a model by its container root; the player lives on the rig
            // descendant. Resolve to it so transport drives the right entity.
            const Entity target = animatableDescendant(scene, *entity);
            if (!hasComponent<AnimationPlayerComponent>(scene, target))
            {
                addComponent<AnimationPlayerComponent>(scene, target);
            }
            return &getComponent<AnimationPlayerComponent>(scene, target);
        }

        auto skeletonOverlayState(const SkeletonOverlayOptions& opts) -> SkeletonOverlayResult
        {
            SkeletonOverlayResult out;
            out.show = opts.show;
            out.axes = opts.axes;
            out.jointSize = opts.jointSize;
            out.highlightJoint = opts.highlightJoint;
            return out;
        }

        auto footIkState(const FootIkComponent& ik) -> FootIkResult
        {
            FootIkResult out;
            out.enabled = ik.enabled;
            out.groundHeight = ik.groundHeight;
            out.chains = static_cast<i32>(ik.chains.size());
            return out;
        }

        // Resolve the entity and fetch its foot-IK config, attaching a default one if absent.
        auto footIkOf(EngineContext& ctx, const EntitySelector& selector) -> Result<FootIkComponent*>
        {
            auto entity = resolveEntity(ctx, json{ { "entity", selector.value } });
            if (!entity)
            {
                return Err(entity.error());
            }
            Scene& scene = activeScene(ctx.sceneEdit);
            // Foot IK lives on the rig descendant too — resolve a selected model root to it.
            const Entity target = animatableDescendant(scene, *entity);
            if (!hasComponent<FootIkComponent>(scene, target))
            {
                addComponent<FootIkComponent>(scene, target);
            }
            return &getComponent<FootIkComponent>(scene, target);
        }

        auto stateOf(EngineContext& ctx, const AnimationPlayerComponent& player) -> AnimationStateResult
        {
            AnimationStateResult out;
            out.clip = WireUuid{ player.clip.value };
            out.clipName = std::string{};
            out.duration = 0.0f;
            if (const AssetEntry* entry = findAsset(ctx.assets.catalog, player.clip); entry != nullptr)
            {
                out.clipName = entry->name;
                out.duration = entry->duration;
            }
            out.time = player.time;
            out.playing = player.playing;
            out.wrap = wrapName(player.wrap);
            out.speed = player.speed;
            out.animationVersion = static_cast<i32>(ctx.sceneEdit.animationVersion);
            return out;
        }
    }

    void registerAnimationCommands(CommandRegistry& reg)
    {
        registerCommand<AnimationStateParams, AnimationStateResult>(
            reg, "get-animation-state", "get-animation-state {entity} — the rig's playhead, clip, wrap, and speed",
            [](EngineContext& ctx, const AnimationStateParams& params) -> Result<AnimationStateResult>
            {
                auto entity = resolveEntity(ctx, json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                // Resolve a model container root to its rig descendant (the player's entity).
                const Entity target = animatableDescendant(scene, *entity);
                if (!hasComponent<AnimationPlayerComponent>(scene, target))
                {
                    return Err(std::string{ "entity has no animation player" });
                }
                return stateOf(ctx, getComponent<AnimationPlayerComponent>(scene, target));
            });

        registerCommand<ListClipsParams, ListClipsResult>(
            reg, "list-clips", "list-clips {asset?} — animation clips: a model container's own, or the full catalog",
            [](EngineContext& ctx, const ListClipsParams& params) -> Result<ListClipsResult>
            {
                Uuid container{ 0 };
                if (params.asset)
                {
                    auto resolved = resolveContainer(ctx, *params.asset);
                    if (!resolved)
                    {
                        return Err(resolved.error());
                    }
                    container = *resolved;
                    if (container.value == 0)
                    {
                        return Err(std::string{ "asset is not part of a model container" });
                    }
                }
                ListClipsResult out;
                for (const AssetEntry& entry : ctx.assets.catalog.entries)
                {
                    if (entry.type != AssetType::Animation)
                    {
                        continue;
                    }
                    if (container.value != 0 && entry.container.value != container.value)
                    {
                        continue;
                    }
                    out.clips.push_back(
                        AnimationClipDto{ WireUuid{ entry.id.value }, entry.name, entry.duration, entry.tracks });
                }
                return out;
            });

        registerCommand<PlayAnimationParams, AnimationStateResult>(
            reg, "play-animation",
            "play-animation {entity, clip, speed=1, loop=true, blend=0} — play a clip (previews in Edit too)",
            [](EngineContext& ctx, const PlayAnimationParams& params) -> Result<AnimationStateResult>
            {
                auto clip = resolveClip(ctx, params.clip);
                if (!clip)
                {
                    return Err(clip.error());
                }
                auto player = playerOf(ctx, params.entity);
                if (!player)
                {
                    return Err(player.error());
                }
                AnimationPlayerComponent& p = **player;
                const f32 blend = params.blend.value_or(0.0f);
                if (blend > 0.0f && p.clip.value != 0 && p.clip.value != (*clip)->id.value)
                {
                    p.prevClip = p.clip;  // cross-fade / inertialize from the current clip
                    p.transition = 0.0f;
                    p.transitionDuration = blend;
                }
                p.clip = (*clip)->id;
                p.time = 0.0f;
                p.speed = params.speed.value_or(1.0f);
                if (params.loop.value_or(true))
                {
                    p.wrap = AnimationPlayerComponent::Wrap::Loop;
                }
                else
                {
                    p.wrap = AnimationPlayerComponent::Wrap::Once;
                }
                // `paused` loads the clip at frame 0 without advancing (UE5's clip-pick semantics); the
                // pose still previews in Edit so the rig shows frame 0.
                p.playing = !params.paused.value_or(false);
                p.previewInEdit = true;
                ctx.sceneEdit.animationVersion += 1;
                return stateOf(ctx, p);
            });

        registerCommand<AnimationStateParams, AnimationStateResult>(
            reg, "pause-animation", "pause-animation {entity} — stop advancing time (keeps the pose shown)",
            [](EngineContext& ctx, const AnimationStateParams& params) -> Result<AnimationStateResult>
            {
                auto player = playerOf(ctx, params.entity);
                if (!player)
                {
                    return Err(player.error());
                }
                (*player)->playing = false;
                ctx.sceneEdit.animationVersion += 1;
                return stateOf(ctx, **player);
            });

        registerCommand<SeekAnimationParams, AnimationStateResult>(
            reg, "seek-animation",
            "seek-animation {entity, time, seekBlend=0} — set the playhead (previews in Edit); seekBlend "
            "eases the pose to the seeked time instead of snapping",
            [](EngineContext& ctx, const SeekAnimationParams& params) -> Result<AnimationStateResult>
            {
                auto player = playerOf(ctx, params.entity);
                if (!player)
                {
                    return Err(player.error());
                }
                (*player)->time = params.time;
                // A self-transition (prevClip == clip) inertializes the pose from where it is now toward
                // the seeked time over `seekBlend` seconds, so sparse scrub seeks read as smooth motion.
                const f32 seekBlend = params.seekBlend.value_or(0.0f);
                if (seekBlend > 0.0f && (*player)->clip.value != 0)
                {
                    (*player)->prevClip = (*player)->clip;
                    (*player)->transition = 0.0f;
                    (*player)->transitionDuration = seekBlend;
                }
                (*player)->previewInEdit = true;
                ctx.sceneEdit.animationVersion += 1;
                return stateOf(ctx, **player);
            });

        registerCommand<SetAnimationLoopParams, AnimationStateResult>(
            reg, "set-animation-loop", "set-animation-loop {entity, wrap} — once | loop | pingpong",
            [](EngineContext& ctx, const SetAnimationLoopParams& params) -> Result<AnimationStateResult>
            {
                auto player = playerOf(ctx, params.entity);
                if (!player)
                {
                    return Err(player.error());
                }
                (*player)->wrap = wrapFromName(params.wrap);
                ctx.sceneEdit.animationVersion += 1;
                return stateOf(ctx, **player);
            });

        registerCommand<AnimationStateParams, AnimationStateResult>(
            reg, "stop-preview", "stop-preview {entity} — clear the Edit preview and stop (revert to rest)",
            [](EngineContext& ctx, const AnimationStateParams& params) -> Result<AnimationStateResult>
            {
                auto player = playerOf(ctx, params.entity);
                if (!player)
                {
                    return Err(player.error());
                }
                (*player)->previewInEdit = false;
                (*player)->playing = false;
                ctx.sceneEdit.animationVersion += 1;
                return stateOf(ctx, **player);
            });

        registerCommand<EmptyParams, SkeletonOverlayResult>(
            reg, "get-skeleton-overlay",
            "get-skeleton-overlay — the line-skeleton overlay toggle, axes, and joint size",
            [](EngineContext& ctx, const EmptyParams&) -> Result<SkeletonOverlayResult>
            { return skeletonOverlayState(ctx.sceneEdit.skeletonOverlay); });

        registerCommand<SetSkeletonOverlayParams, SkeletonOverlayResult>(
            reg, "set-skeleton-overlay",
            "set-skeleton-overlay {show?, axes?, jointSize?} — the selected rig's line-skeleton viewport overlay",
            [](EngineContext& ctx, const SetSkeletonOverlayParams& params) -> Result<SkeletonOverlayResult>
            {
                SkeletonOverlayOptions& opts = ctx.sceneEdit.skeletonOverlay;
                if (params.show)
                {
                    opts.show = *params.show;
                }
                if (params.axes)
                {
                    opts.axes = *params.axes;
                }
                if (params.jointSize)
                {
                    opts.jointSize = std::max(0.5f, *params.jointSize);
                }
                return skeletonOverlayState(opts);
            });

        // Tint one joint of the previewed model's overlay, addressed by its get-asset-model node index.
        // The asset editor's skeleton tree drives this instead of scene selection — selecting a bone
        // entity would null the selection-keyed animation state and break the timeline. -1 clears it.
        registerCommand<SetSkeletonHighlightParams, SkeletonOverlayResult>(
            reg, "set-skeleton-highlight",
            "set-skeleton-highlight {joint} — tint a previewed model's joint by its get-asset-model node index",
            [](EngineContext& ctx, const SetSkeletonHighlightParams& params) -> Result<SkeletonOverlayResult>
            {
                i32 highlightJoint = params.joint;
                if (params.joint < 0)
                {
                    highlightJoint = -1;
                }
                ctx.sceneEdit.skeletonOverlay.highlightJoint = highlightJoint;
                return skeletonOverlayState(ctx.sceneEdit.skeletonOverlay);
            });

        // Reverse selection for the asset editor: project the previewed model's joints to the viewport
        // and return the node index of the one nearest the click (within radiusPx), so clicking a joint
        // dot in the viewport selects its bone in the skeleton tree. The overlay joints are pure geometry
        // (the spawned bone entities carry no mesh, so the generic `pick` skips them), hence a dedicated
        // screen-space hit-test against the same world positions the overlay draws.
        registerCommand<PickSkeletonJointParams, PickSkeletonJointResult>(
            reg, "pick-skeleton-joint",
            "pick-skeleton-joint {u, v, radiusPx=8} — the previewed model's nearest joint to a viewport click",
            [](EngineContext& ctx, const PickSkeletonJointParams& params) -> Result<PickSkeletonJointResult>
            {
                SceneEditContext& edit = ctx.sceneEdit;
                if (!previewing(edit) || edit.previewBoneByNode.empty())
                {
                    return PickSkeletonJointResult{ false, -1 };
                }
                const u32 width = viewportWidth(ctx.renderer);
                const u32 height = viewportHeight(ctx.renderer);
                if (width == 0 || height == 0)
                {
                    return PickSkeletonJointResult{ false, -1 };
                }
                Scene& scene = activeScene(edit);
                updateWorldTransforms(scene);  // pick against the same world joint positions the overlay draws
                const CameraView cam = renderCameraView(edit);
                const glm::vec2 clickPx{ params.u * static_cast<f32>(width), params.v * static_cast<f32>(height) };
                const f32 radiusPx = params.radiusPx.value_or(8.0f);
                i32 bestNode = -1;
                f32 bestDistSq = radiusPx * radiusPx;
                for (std::size_t node = 0; node < edit.previewBoneByNode.size(); node = node + 1)
                {
                    const Uuid id = edit.previewBoneByNode[node];
                    if (id.value == 0)
                    {
                        continue;
                    }
                    const Entity bone = findEntityByUuid(scene, id.value);
                    if (bone.handle == entt::null || !valid(scene, bone))
                    {
                        continue;
                    }
                    const GizmoProjection p = viewportProject(cam, width, height, worldTranslation(scene, bone));
                    if (!p.visible)
                    {
                        continue;
                    }
                    const glm::vec2 d = p.pixel - clickPx;
                    const f32 distSq = glm::dot(d, d);
                    if (distSq <= bestDistSq)
                    {
                        bestDistSq = distSq;
                        bestNode = static_cast<i32>(node);
                    }
                }
                return PickSkeletonJointResult{ bestNode >= 0, bestNode };
            });

        registerCommand<GetFootIkParams, FootIkResult>(
            reg, "get-foot-ik", "get-foot-ik {entity} — the rig's foot-IK enable, ground height, and chain count",
            [](EngineContext& ctx, const GetFootIkParams& params) -> Result<FootIkResult>
            {
                auto ik = footIkOf(ctx, params.entity);
                if (!ik)
                {
                    return Err(ik.error());
                }
                return footIkState(**ik);
            });

        registerCommand<SetFootIkParams, FootIkResult>(
            reg, "set-foot-ik", "set-foot-ik {entity, enabled?, groundHeight?} — toggle kinematic foot IK on a rig",
            [](EngineContext& ctx, const SetFootIkParams& params) -> Result<FootIkResult>
            {
                auto ik = footIkOf(ctx, params.entity);
                if (!ik)
                {
                    return Err(ik.error());
                }
                FootIkComponent& c = **ik;
                if (params.enabled)
                {
                    c.enabled = *params.enabled;
                }
                if (params.groundHeight)
                {
                    c.groundHeight = *params.groundHeight;
                }
                ctx.sceneEdit.animationVersion += 1;
                return footIkState(c);
            });
    }
}
