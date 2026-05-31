module;

#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder for the default editor layout
#include <ImGuizmo.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <string>

export module Saffron.Ui;

import Saffron.Core;
import Saffron.Window;
import Saffron.Rendering;

export namespace se
{
    struct Ui
    {
        vk::DescriptorPool descriptorPool;
        ImTextureID viewportTexture = 0;   // ImGui handle for the offscreen image
        u32 knownViewportGeneration = 0;   // offscreen generation we last registered
        bool initialized = false;
        ImVec2 viewportPos{};              // screen-space rect of the viewport image,
        ImVec2 viewportSize{};             // captured each frame for gizmo overlay
        bool viewportHovered = false;
        ImFont* monoFont = nullptr;        // Roboto Mono for numeric/data fields
        bool layoutBuilt = false;          // seeded the default dock layout once
    };

    std::expected<Ui, std::string> newUi(Renderer& renderer, Window& window);
    void destroyUi(Renderer& renderer, Ui& ui);

    void uiBeginFrame(Ui& ui);                  // NewFrame + dockspace host
    void uiEndFrame(Ui& ui);                    // ImGui::Render()
    void viewportPanel(Ui& ui, Renderer& renderer);  // dockable scene view
    void uiRecordDrawData(Renderer& renderer);  // submit draw data into the frame

    // The viewport image's screen rect + hover state, valid after viewportPanel ran
    // this frame — used to place the ImGuizmo overlay.
    ImVec2 viewportContentPos(const Ui& ui);
    ImVec2 viewportContentSize(const Ui& ui);
    bool viewportHovered(const Ui& ui);

    // The Roboto Mono font for numeric/data fields, or null if it failed to load.
    ImFont* uiMonoFont(const Ui& ui);
}

namespace se
{
    std::expected<Ui, std::string> newUi(Renderer& renderer, Window& window)
    {
        Ui ui;

        std::array<vk::DescriptorPoolSize, 3> poolSizes{
            vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, 1000 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eSampler, 1000 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eSampledImage, 1000 },
        };
        vk::DescriptorPoolCreateInfo poolInfo{};
        poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        poolInfo.maxSets = 1000;
        poolInfo.setPoolSizes(poolSizes);
        vk::ResultValue<vk::DescriptorPool> pool = renderer.device.createDescriptorPool(poolInfo);
        if (pool.result != vk::Result::eSuccess)
        {
            return std::unexpected(std::format("createDescriptorPool: {}", vk::to_string(pool.result)));
        }
        ui.descriptorPool = pool.value;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();

        // Roboto for the UI, Roboto Mono for numeric/data fields. Optional — fall back
        // to the built-in font if the files are missing.
        const std::string robotoPath = assetPath("fonts/Roboto-Regular.ttf");
        if (std::filesystem::exists(robotoPath))
        {
            io.FontDefault = io.Fonts->AddFontFromFileTTF(robotoPath.c_str(), 17.0f);
        }
        const std::string monoPath = assetPath("fonts/RobotoMono-Regular.ttf");
        if (std::filesystem::exists(monoPath))
        {
            ui.monoFont = io.Fonts->AddFontFromFileTTF(monoPath.c_str(), 16.0f);
        }

        if (!ImGui_ImplSDL3_InitForVulkan(window.handle))
        {
            return std::unexpected(std::string{ "ImGui_ImplSDL3_InitForVulkan failed" });
        }

        // ImGui consumes the color format during Init (when it builds its pipeline),
        // so a local is fine — the pointer is not read afterwards.
        VkFormat colorFormat = static_cast<VkFormat>(renderer.swapchainFormat);

        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_3;
        init.Instance = renderer.instance;
        init.PhysicalDevice = renderer.physicalDevice;
        init.Device = renderer.device;
        init.QueueFamily = renderer.graphicsQueueFamily;
        init.Queue = renderer.graphicsQueue;
        init.DescriptorPool = ui.descriptorPool;
        init.MinImageCount = 2;
        init.ImageCount = static_cast<u32>(renderer.swapchainImages.size());
        init.UseDynamicRendering = true;
        init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init.PipelineInfoMain.PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
        if (!ImGui_ImplVulkan_Init(&init))
        {
            return std::unexpected(std::string{ "ImGui_ImplVulkan_Init failed" });
        }

        window.eventSinks.push_back([](const SDL_Event& event) { ImGui_ImplSDL3_ProcessEvent(&event); });

        // Register the offscreen viewport image as an ImGui texture (once; refreshed
        // in viewportPanel only when the renderer recreates the image).
        ui.viewportTexture = (ImTextureID)ImGui_ImplVulkan_AddTexture(
            viewportImageView(renderer), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ui.knownViewportGeneration = viewportGeneration(renderer);

        ui.initialized = true;
        logInfo("imgui ready — docking enabled");
        return ui;
    }

    void destroyUi(Renderer& renderer, Ui& ui)
    {
        if (!ui.initialized)
        {
            return;
        }
        static_cast<void>(renderer.device.waitIdle());
        if (ui.viewportTexture != 0)
        {
            ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)ui.viewportTexture);
            ui.viewportTexture = 0;
        }
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        renderer.device.destroyDescriptorPool(ui.descriptorPool);
        ui.descriptorPool = nullptr;
        ui.initialized = false;
    }

    void uiBeginFrame(Ui& ui)
    {
        static_cast<void>(ui);
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();

        const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (!ui.layoutBuilt)
        {
            ui.layoutBuilt = true;
            ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
            if (node == nullptr || node->IsLeafNode())  // empty (no saved layout) → seed a default
            {
                ImGui::DockBuilderRemoveNode(dockId);
                ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->Size);
                ImGuiID center = dockId;
                const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
                const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
                const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.55f, nullptr, nullptr);
                ImGui::DockBuilderDockWindow("Hierarchy", left);
                ImGui::DockBuilderDockWindow("Inspector", leftBottom);
                ImGui::DockBuilderDockWindow("Assets", bottom);
                ImGui::DockBuilderDockWindow("Viewport", center);
                ImGui::DockBuilderFinish(dockId);
            }
        }
    }

    void uiEndFrame(Ui& ui)
    {
        static_cast<void>(ui);
        ImGui::Render();
    }

    void viewportPanel(Ui& ui, Renderer& renderer)
    {
        ImGui::SetNextWindowSize(ImVec2{ 1280.0f, 720.0f }, ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0.0f, 0.0f });
        ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoTitleBar);
        ImGui::PopStyleVar();

        // Request the offscreen image be sized to the panel (in pixels). The
        // renderer applies the resize at the start of the next frame.
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        if (scale.x > 0.0f) { scaleX = scale.x; }
        if (scale.y > 0.0f) { scaleY = scale.y; }
        setViewportDesiredSize(renderer,
                               static_cast<u32>(avail.x * scaleX),
                               static_cast<u32>(avail.y * scaleY));

        // Re-register the texture when the renderer recreated the offscreen image.
        // Safe: the recreate path issues a full device idle before this runs.
        if (viewportGeneration(renderer) != ui.knownViewportGeneration)
        {
            if (ui.viewportTexture != 0)
            {
                ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)ui.viewportTexture);
            }
            ui.viewportTexture = (ImTextureID)ImGui_ImplVulkan_AddTexture(
                viewportImageView(renderer), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            ui.knownViewportGeneration = viewportGeneration(renderer);
        }

        if (ui.viewportTexture != 0 && avail.x > 0.0f && avail.y > 0.0f)
        {
            ImGui::Image(ui.viewportTexture, avail);  // logical size; image is pixel-sized
            ui.viewportPos = ImGui::GetItemRectMin();
            ui.viewportSize = ImGui::GetItemRectSize();
            ui.viewportHovered = ImGui::IsItemHovered();
        }
        ImGui::End();
    }

    ImVec2 viewportContentPos(const Ui& ui)
    {
        return ui.viewportPos;
    }

    ImVec2 viewportContentSize(const Ui& ui)
    {
        return ui.viewportSize;
    }

    bool viewportHovered(const Ui& ui)
    {
        return ui.viewportHovered;
    }

    ImFont* uiMonoFont(const Ui& ui)
    {
        return ui.monoFont;
    }

    void uiRecordDrawData(Renderer& renderer)
    {
        submitUi(renderer, [](vk::CommandBuffer cmd)
        {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<VkCommandBuffer>(cmd));
        });
    }
}
