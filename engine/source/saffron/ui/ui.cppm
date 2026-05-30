module;

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include <expected>
#include <string>

export module Saffron.Ui;

import Saffron.Core;
import Saffron.Window;
import Saffron.Rendering;

export namespace se
{
    struct Ui
    {
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        bool initialized = false;
    };

    std::expected<Ui, std::string> newUi(Renderer& renderer, Window& window);
    void destroyUi(Renderer& renderer, Ui& ui);

    void uiBeginFrame(Ui& ui);                  // NewFrame + dockspace host
    void uiEndFrame(Ui& ui);                    // ImGui::Render()
    void uiRecordDrawData(Renderer& renderer);  // submit draw data into the frame
}

namespace se
{
    std::expected<Ui, std::string> newUi(Renderer& renderer, Window& window)
    {
        Ui ui;

        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        };
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 1000;
        poolInfo.poolSizeCount = static_cast<u32>(std::size(poolSizes));
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(renderer.device, &poolInfo, nullptr, &ui.descriptorPool) != VK_SUCCESS)
        {
            return std::unexpected(std::string{ "failed to create ImGui descriptor pool" });
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();

        if (!ImGui_ImplSDL3_InitForVulkan(window.handle))
        {
            return std::unexpected(std::string{ "ImGui_ImplSDL3_InitForVulkan failed" });
        }

        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_3;
        init.Instance = renderer.vkbInstance.instance;
        init.PhysicalDevice = renderer.physicalDevice;
        init.Device = renderer.device;
        init.QueueFamily = renderer.graphicsQueueFamily;
        init.Queue = renderer.graphicsQueue;
        init.DescriptorPool = ui.descriptorPool;
        init.MinImageCount = 2;
        init.ImageCount = static_cast<u32>(renderer.swapchainImages.size());
        init.UseDynamicRendering = true;
        // ImGui 1.92.8 moved the dynamic-rendering pipeline config into PipelineInfoMain.
        init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init.PipelineInfoMain.PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &renderer.swapchainFormat;
        if (!ImGui_ImplVulkan_Init(&init))
        {
            return std::unexpected(std::string{ "ImGui_ImplVulkan_Init failed" });
        }

        // Feed SDL events to ImGui without the window module knowing about ImGui.
        window.eventSinks.push_back([](const SDL_Event& event) { ImGui_ImplSDL3_ProcessEvent(&event); });

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
        vkDeviceWaitIdle(renderer.device);
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(renderer.device, ui.descriptorPool, nullptr);
        ui.descriptorPool = VK_NULL_HANDLE;
        ui.initialized = false;
    }

    void uiBeginFrame(Ui& ui)
    {
        static_cast<void>(ui);
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport();
    }

    void uiEndFrame(Ui& ui)
    {
        static_cast<void>(ui);
        ImGui::Render();
    }

    void uiRecordDrawData(Renderer& renderer)
    {
        submit(renderer, [](VkCommandBuffer cmd) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        });
    }
}
