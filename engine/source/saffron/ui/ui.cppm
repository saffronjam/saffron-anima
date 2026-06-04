module;

#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder for the default editor layout; GImGui for vec3Control
#include <ImGuizmo.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

export module Saffron.Ui;

import Saffron.Core;
import Saffron.Window;
import Saffron.Rendering;

export namespace se
{
    namespace theme
    {
        constexpr ImU32 accent            = IM_COL32(236, 158,  36, 255);
        constexpr ImU32 highlight         = IM_COL32( 39, 185, 242, 255);
        constexpr ImU32 niceBlue          = IM_COL32( 83, 232, 254, 255);
        constexpr ImU32 compliment        = IM_COL32( 78, 151, 166, 255);
        constexpr ImU32 background        = IM_COL32( 36,  36,  36, 255);
        constexpr ImU32 backgroundDark    = IM_COL32( 26,  26,  26, 255);
        constexpr ImU32 titlebar          = IM_COL32( 21,  21,  21, 255);
        constexpr ImU32 propertyField     = IM_COL32( 15,  15,  15, 255);
        constexpr ImU32 text              = IM_COL32(192, 192, 192, 255);
        constexpr ImU32 textBrighter      = IM_COL32(210, 210, 210, 255);
        constexpr ImU32 textDarker        = IM_COL32(128, 128, 128, 255);
        constexpr ImU32 muted             = IM_COL32( 77,  77,  77, 255);
        constexpr ImU32 groupHeader       = IM_COL32( 47,  47,  47, 255);
        constexpr ImU32 selection         = IM_COL32(237, 192, 119, 255);
        constexpr ImU32 selectionMuted    = IM_COL32(237, 201, 142,  23);
        constexpr ImU32 backgroundPopup   = IM_COL32( 63,  70,  77, 255);
    }

    // RAII ImGui::PushStyleVar — one var, one value.
    struct StyleBinder
    {
        template<typename T>
        StyleBinder(ImGuiStyleVar var, T val) { ImGui::PushStyleVar(var, val); }
        ~StyleBinder() { ImGui::PopStyleVar(); }
        StyleBinder(const StyleBinder&) = delete;
        StyleBinder& operator=(const StyleBinder&) = delete;
    };

    // RAII ImGui::PushStyleColor — one slot, one color.
    struct ColorBinder
    {
        template<typename T>
        ColorBinder(ImGuiCol col, T val) { ImGui::PushStyleColor(col, val); }
        ~ColorBinder() { ImGui::PopStyleColor(); }
        ColorBinder(const ColorBinder&) = delete;
        ColorBinder& operator=(const ColorBinder&) = delete;
    };

    // RAII variadic ImGui::PushStyleColor — takes (ImGuiCol, color, ImGuiCol, color, …) pairs.
    struct ColorStack
    {
        template<typename T, typename... Pairs>
        ColorStack(ImGuiCol col, T color, Pairs&&... pairs)
            : count(static_cast<int>(sizeof...(pairs) / 2) + 1)
        {
            static_assert(sizeof...(pairs) % 2 == 0, "ColorStack requires (ImGuiCol, color) pairs");
            push(col, color, std::forward<Pairs>(pairs)...);
        }
        ~ColorStack() { ImGui::PopStyleColor(count); }
        ColorStack(const ColorStack&) = delete;
        ColorStack& operator=(const ColorStack&) = delete;
    private:
        int count;
        template<typename T, typename... Pairs>
        void push(ImGuiCol col, T color, Pairs&&... pairs)
        {
            ImGui::PushStyleColor(col, color);
            if constexpr (sizeof...(pairs) > 0) { push(std::forward<Pairs>(pairs)...); }
        }
    };

    // RAII variadic ImGui::PushStyleVar — takes (ImGuiStyleVar, value, …) pairs.
    struct StyleStack
    {
        template<typename T, typename... Pairs>
        StyleStack(ImGuiStyleVar var, T val, Pairs&&... pairs)
            : count(static_cast<int>(sizeof...(pairs) / 2) + 1)
        {
            static_assert(sizeof...(pairs) % 2 == 0, "StyleStack requires (ImGuiStyleVar, value) pairs");
            push(var, val, std::forward<Pairs>(pairs)...);
        }
        ~StyleStack() { ImGui::PopStyleVar(count); }
        StyleStack(const StyleStack&) = delete;
        StyleStack& operator=(const StyleStack&) = delete;
    private:
        int count;
        template<typename T, typename... Pairs>
        void push(ImGuiStyleVar var, T val, Pairs&&... pairs)
        {
            ImGui::PushStyleVar(var, val);
            if constexpr (sizeof...(pairs) > 0) { push(std::forward<Pairs>(pairs)...); }
        }
    };

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

    auto newUi(Renderer& renderer, Window& window) -> Result<Ui>;
    void destroyUi(Renderer& renderer, Ui& ui);

    void uiBeginFrame(Ui& ui);                  // NewFrame + dockspace host
    void uiEndFrame(Ui& ui);                    // ImGui::Render()
    void viewportPanel(Ui& ui, Renderer& renderer);  // dockable scene view
    void uiRecordDrawData(Renderer& renderer);  // submit draw data into the frame

    // The viewport image's screen rect + hover state, valid after viewportPanel ran
    // this frame — used to place the ImGuizmo overlay.
    auto viewportContentPos(const Ui& ui) -> ImVec2;
    auto viewportContentSize(const Ui& ui) -> ImVec2;
    auto viewportHovered(const Ui& ui) -> bool;

    // The Roboto Mono font for numeric/data fields, or null if it failed to load.
    auto uiMonoFont(const Ui& ui) -> ImFont*;

    // Registers a GPU texture with the ImGui Vulkan backend for display (ImGui::Image).
    // Returns an ImTextureID (0 if the texture is null); free it with uiUnregisterTexture.
    auto uiRegisterTexture(const Ref<GpuTexture>& texture) -> ImTextureID;
    void uiUnregisterTexture(ImTextureID texture);

    // Applies the Saffron dark theme. Called once during newUi() after CreateContext().
    void applyTheme();

    // Cursor layout helpers.
    void shiftCursorX(float distance);
    void shiftCursorY(float distance);
    void shiftCursor(float x, float y);

    // Draws a 1px horizontal rule below the current cursor position.
    void underline(bool fullWidth = false, float offsetX = 0.0f, float offsetY = -1.0f);

    // Three-axis drag control (X/Y/Z coloured reset buttons + DragFloat per axis).
    // `values` is a pointer to 3 contiguous floats (e.g. &glm::vec3::x).
    // Returns true if any axis was modified.
    bool vec3Control(std::string_view label, float* values, float resetValue = 0.0f);

    // Framed, uppercase collapsing section header. Caller must call ImGui::TreePop()
    // when the returned bool is true.
    bool propertyGridHeader(std::string_view name, bool openByDefault = true);

    // Popup wrapper that fills its background with a subtle gradient.
    bool beginPopup(const char* id, ImGuiWindowFlags flags = 0);
    void endPopup();

    // HSV channel modifiers — convert an ImColor, replace one channel, return ImU32.
    auto colourWithValue(ImColor color, float value) -> ImU32;
    auto colourWithSaturation(ImColor color, float saturation) -> ImU32;
    auto colourWithHue(ImColor color, float hue) -> ImU32;
    auto colourWithMultipliedValue(ImColor color, float multiplier) -> ImU32;
    auto colourWithMultipliedSaturation(ImColor color, float multiplier) -> ImU32;
    auto colourWithMultipliedHue(ImColor color, float multiplier) -> ImU32;

    // ImRect geometry helpers (imgui_internal.h types).
    auto getItemRect() -> ImRect;
    auto rectExpanded(ImRect rect, float x, float y) -> ImRect;
    auto rectOffset(ImRect rect, float x, float y) -> ImRect;
    auto rectOffset(ImRect rect, ImVec2 xy) -> ImRect;
}

namespace se
{
    void applyTheme()
    {
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        auto* colors = style.Colors;

        auto u32 = [](ImU32 v) { return ImGui::ColorConvertU32ToFloat4(v); };

        colors[ImGuiCol_WindowBg]             = u32(theme::background);
        colors[ImGuiCol_ChildBg]              = u32(theme::background);
        colors[ImGuiCol_PopupBg]              = u32(theme::backgroundPopup);

        colors[ImGuiCol_MenuBarBg]            = u32(theme::titlebar);
        colors[ImGuiCol_TitleBg]              = u32(theme::titlebar);
        colors[ImGuiCol_TitleBgActive]        = u32(theme::titlebar);
        colors[ImGuiCol_TitleBgCollapsed]     = u32(theme::titlebar);

        colors[ImGuiCol_FrameBg]              = u32(theme::propertyField);
        colors[ImGuiCol_FrameBgHovered]       = u32(theme::groupHeader);
        colors[ImGuiCol_FrameBgActive]        = u32(theme::muted);

        colors[ImGuiCol_Header]               = u32(theme::groupHeader);
        colors[ImGuiCol_HeaderHovered]        = u32(theme::muted);
        colors[ImGuiCol_HeaderActive]         = u32(theme::backgroundDark);

        colors[ImGuiCol_Button]               = u32(theme::muted);
        colors[ImGuiCol_ButtonHovered]        = u32(theme::groupHeader);
        colors[ImGuiCol_ButtonActive]         = u32(theme::backgroundDark);

        colors[ImGuiCol_Tab]                  = u32(theme::titlebar);
        colors[ImGuiCol_TabHovered]           = u32(theme::muted);
        colors[ImGuiCol_TabSelected]          = u32(theme::background);
        colors[ImGuiCol_TabDimmed]            = u32(theme::titlebar);
        colors[ImGuiCol_TabDimmedSelected]    = u32(theme::backgroundDark);

        colors[ImGuiCol_ScrollbarBg]          = u32(theme::backgroundDark);
        colors[ImGuiCol_ScrollbarGrab]        = u32(theme::muted);
        colors[ImGuiCol_ScrollbarGrabHovered] = u32(theme::groupHeader);
        colors[ImGuiCol_ScrollbarGrabActive]  = u32(theme::accent);

        colors[ImGuiCol_Border]               = u32(theme::muted);

        colors[ImGuiCol_Text]                 = u32(theme::text);
        colors[ImGuiCol_TextDisabled]         = u32(theme::textDarker);

        colors[ImGuiCol_CheckMark]            = u32(theme::accent);
        colors[ImGuiCol_SliderGrab]           = u32(theme::accent);
        colors[ImGuiCol_SliderGrabActive]     = u32(theme::highlight);

        colors[ImGuiCol_Separator]            = u32(theme::backgroundDark);
        colors[ImGuiCol_SeparatorHovered]     = u32(theme::highlight);
        colors[ImGuiCol_SeparatorActive]      = u32(theme::accent);

        colors[ImGuiCol_TextSelectedBg]       = u32(theme::selectionMuted);
        colors[ImGuiCol_NavHighlight]         = u32(theme::accent);

        colors[ImGuiCol_DockingPreview]       = u32(theme::accent);
        colors[ImGuiCol_DockingEmptyBg]       = u32(theme::backgroundDark);

        style.FrameRounding   = 2.5f;
        style.FrameBorderSize = 1.0f;
        style.WindowBorderSize = 1.0f;
        style.TabRounding     = 3.5f;
        style.IndentSpacing   = 11.0f;
    }

    void shiftCursorX(float distance)
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + distance);
    }

    void shiftCursorY(float distance)
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + distance);
    }

    void shiftCursor(float x, float y)
    {
        ImVec2 cursor = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(cursor.x + x, cursor.y + y));
    }

    void underline(bool fullWidth, float offsetX, float offsetY)
    {
        if (fullWidth)
        {
            if (ImGui::GetCurrentWindow()->DC.CurrentColumns != nullptr)
                ImGui::PushColumnsBackground();
            else if (ImGui::GetCurrentTable() != nullptr)
                ImGui::TablePushBackgroundChannel();
        }
        const float width = fullWidth ? ImGui::GetWindowWidth() : ImGui::GetContentRegionAvail().x;
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(cursor.x + offsetX, cursor.y + offsetY),
            ImVec2(cursor.x + width,   cursor.y + offsetY),
            theme::backgroundDark, 1.0f);
        if (fullWidth)
        {
            if (ImGui::GetCurrentWindow()->DC.CurrentColumns != nullptr)
                ImGui::PopColumnsBackground();
            else if (ImGui::GetCurrentTable() != nullptr)
                ImGui::TablePopBackgroundChannel();
        }
    }

    bool vec3Control(std::string_view label, float* values, float resetValue)
    {
        bool modified = false;
        ImGui::PushID(label.data());

        shiftCursorY(2.0f);
        ImGui::TextUnformatted(label.data());
        underline(false, 0.0f, 2.0f);
        shiftCursorY(3.0f);

        const float spacingX    = 4.0f;
        const float framePad    = 3.0f;
        const float lineHeight  = ImGui::GetFontSize() + framePad * 2.0f;
        const ImVec2 btnSize    = { lineHeight + 2.0f, lineHeight };
        const float avail       = ImGui::GetContentRegionAvail().x;
        const float inputWidth  = (avail - spacingX * 2.0f) / 3.0f - btnSize.x;

        StyleBinder spacing(ImGuiStyleVar_ItemSpacing, ImVec2{ spacingX, 0.0f });

        auto drawAxis = [&](const char* axisLabel, float& value,
                            const ImVec4& colN, const ImVec4& colH, const ImVec4& colP)
        {
            StyleBinder btnPad(ImGuiStyleVar_FramePadding, ImVec2{ framePad, framePad });
            StyleBinder btnRound(ImGuiStyleVar_FrameRounding, 1.0f);
            ColorStack btnColors(ImGuiCol_Button,        colN,
                                 ImGuiCol_ButtonHovered, colH,
                                 ImGuiCol_ButtonActive,  colP);
            if (ImGui::Button(axisLabel, btnSize)) { value = resetValue; modified = true; }
            ImGui::SameLine(0.0f, 1.0f);
            ImGui::SetNextItemWidth(inputWidth);
            const std::string dragId = std::string("##") + axisLabel;
            modified |= ImGui::DragFloat(dragId.c_str(), &value, 0.1f, 0.0f, 0.0f, "%.2f");
        };

        drawAxis("X", values[0], {0.8f,0.1f,0.15f,1.0f}, {0.9f,0.2f,0.2f,1.0f}, {0.8f,0.1f,0.15f,1.0f});
        ImGui::SameLine(0.0f, spacingX);
        drawAxis("Y", values[1], {0.2f,0.7f,0.2f,1.0f},  {0.3f,0.8f,0.3f,1.0f}, {0.2f,0.7f,0.2f,1.0f});
        ImGui::SameLine(0.0f, spacingX);
        drawAxis("Z", values[2], {0.1f,0.25f,0.8f,1.0f}, {0.2f,0.35f,0.9f,1.0f},{0.1f,0.25f,0.8f,1.0f});

        ImGui::PopID();
        return modified;
    }

    bool propertyGridHeader(std::string_view name, bool openByDefault)
    {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed
                                 | ImGuiTreeNodeFlags_SpanAvailWidth
                                 | ImGuiTreeNodeFlags_AllowOverlap
                                 | ImGuiTreeNodeFlags_FramePadding;
        if (openByDefault) { flags |= ImGuiTreeNodeFlags_DefaultOpen; }

        StyleBinder rounding(ImGuiStyleVar_FrameRounding, 0.0f);
        StyleBinder padding(ImGuiStyleVar_FramePadding, ImVec2{ 6.0f, 6.0f });

        std::string upper(name);
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        ImGui::PushID(name.data());
        const bool open = ImGui::TreeNodeEx("##hdr", flags, "%s", upper.c_str());
        ImGui::PopID();
        return open;
    }

    bool beginPopup(const char* id, ImGuiWindowFlags flags)
    {
        if (!ImGui::BeginPopup(id, flags)) { return false; }
        const float padding = ImGui::GetStyle().WindowBorderSize;
        const ImRect win = rectExpanded(ImGui::GetCurrentWindow()->Rect(), -padding, -padding);
        ImGui::PushClipRect(win.Min, win.Max, false);
        const ImColor col1 = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        const ImColor col2 = colourWithMultipliedValue(col1, 0.8f);
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(win.Min, win.Max, col1, col1, col2, col2);
        ImGui::GetWindowDrawList()->AddRect(win.Min, win.Max, colourWithMultipliedValue(col1, 1.1f));
        ImGui::PopClipRect();
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 80));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.0f, 1.0f));
        return true;
    }

    void endPopup()
    {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    auto colourWithValue(ImColor color, float value) -> ImU32
    {
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(color.Value.x, color.Value.y, color.Value.z, h, s, v);
        return ImColor::HSV(h, s, std::min(value, 1.0f));
    }

    auto colourWithSaturation(ImColor color, float saturation) -> ImU32
    {
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(color.Value.x, color.Value.y, color.Value.z, h, s, v);
        return ImColor::HSV(h, std::min(saturation, 1.0f), v);
    }

    auto colourWithHue(ImColor color, float hue) -> ImU32
    {
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(color.Value.x, color.Value.y, color.Value.z, h, s, v);
        return ImColor::HSV(std::min(hue, 1.0f), s, v);
    }

    auto colourWithMultipliedValue(ImColor color, float multiplier) -> ImU32
    {
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(color.Value.x, color.Value.y, color.Value.z, h, s, v);
        return ImColor::HSV(h, s, std::min(v * multiplier, 1.0f));
    }

    auto colourWithMultipliedSaturation(ImColor color, float multiplier) -> ImU32
    {
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(color.Value.x, color.Value.y, color.Value.z, h, s, v);
        return ImColor::HSV(h, std::min(s * multiplier, 1.0f), v);
    }

    auto colourWithMultipliedHue(ImColor color, float multiplier) -> ImU32
    {
        float h, s, v;
        ImGui::ColorConvertRGBtoHSV(color.Value.x, color.Value.y, color.Value.z, h, s, v);
        return ImColor::HSV(std::min(h * multiplier, 1.0f), s, v);
    }

    auto getItemRect() -> ImRect
    {
        return ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    auto rectExpanded(ImRect rect, float x, float y) -> ImRect
    {
        rect.Min.x -= x; rect.Min.y -= y;
        rect.Max.x += x; rect.Max.y += y;
        return rect;
    }

    auto rectOffset(ImRect rect, float x, float y) -> ImRect
    {
        rect.Min.x += x; rect.Min.y += y;
        rect.Max.x += x; rect.Max.y += y;
        return rect;
    }

    auto rectOffset(ImRect rect, ImVec2 xy) -> ImRect
    {
        return rectOffset(rect, xy.x, xy.y);
    }

    auto newUi(Renderer& renderer, Window& window) -> Result<Ui>
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
        vk::ResultValue<vk::DescriptorPool> pool = renderer.context.device.createDescriptorPool(poolInfo);
        if (pool.result != vk::Result::eSuccess)
        {
            return Err(std::format("createDescriptorPool: {}", vk::to_string(pool.result)));
        }
        ui.descriptorPool = pool.value;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        applyTheme();

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
            return Err(std::string{ "ImGui_ImplSDL3_InitForVulkan failed" });
        }

        // ImGui consumes the color format during Init (when it builds its pipeline),
        // so a local is fine — the pointer is not read afterwards.
        VkFormat colorFormat = static_cast<VkFormat>(renderer.swapchain.format);

        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_4;
        init.Instance = renderer.context.instance;
        init.PhysicalDevice = renderer.context.physicalDevice;
        init.Device = renderer.context.device;
        init.QueueFamily = renderer.context.graphicsQueueFamily;
        init.Queue = renderer.context.graphicsQueue;
        init.DescriptorPool = ui.descriptorPool;
        init.MinImageCount = 2;
        init.ImageCount = static_cast<u32>(renderer.swapchain.images.size());
        init.UseDynamicRendering = true;
        init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init.PipelineInfoMain.PipelineRenderingCreateInfo = VkPipelineRenderingCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
        if (!ImGui_ImplVulkan_Init(&init))
        {
            return Err(std::string{ "ImGui_ImplVulkan_Init failed" });
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
        if (!ui.initialized) { return; }
        static_cast<void>(renderer.context.device.waitIdle());
        if (ui.viewportTexture != 0)
        {
            ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)ui.viewportTexture);
            ui.viewportTexture = 0;
        }
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        renderer.context.device.destroyDescriptorPool(ui.descriptorPool);
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
                const ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
                const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
                const ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.55f, nullptr, nullptr);
                ImGui::DockBuilderDockWindow("Hierarchy", left);
                ImGui::DockBuilderDockWindow("Inspector", leftBottom);
                ImGui::DockBuilderDockWindow("Environment", leftBottom);
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

    auto viewportContentPos(const Ui& ui) -> ImVec2  { return ui.viewportPos; }
    auto viewportContentSize(const Ui& ui) -> ImVec2 { return ui.viewportSize; }
    auto viewportHovered(const Ui& ui) -> bool        { return ui.viewportHovered; }
    auto uiMonoFont(const Ui& ui) -> ImFont*           { return ui.monoFont; }

    auto uiRegisterTexture(const Ref<GpuTexture>& texture) -> ImTextureID
    {
        if (!texture) { return 0; }
        return (ImTextureID)ImGui_ImplVulkan_AddTexture(
            static_cast<VkImageView>(texture->view), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void uiUnregisterTexture(ImTextureID texture)
    {
        if (texture != 0) { ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)texture); }
    }

    void uiRecordDrawData(Renderer& renderer)
    {
        submitUi(renderer, [](vk::CommandBuffer cmd)
        {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<VkCommandBuffer>(cmd));
        });
    }
}
