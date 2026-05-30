# Third-party dependencies. Libraries are vendored statically via FetchContent
# (pinned tags) except SDL3 and the Vulkan headers/loader, which come from the
# system (saffron-build toolbox). All current as of 2026-05.

include(FetchContent)

# --- System packages ----------------------------------------------------------
find_package(Vulkan REQUIRED)          # Vulkan headers + loader (we use the raw C API, not vulkan.hpp/raii)
find_package(SDL3 REQUIRED CONFIG)     # SDL3 3.4.x, C ABI

# --- Header-only / source libraries (built from source, static) ---------------
FetchContent_Declare(EnTT
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.16.0 GIT_SHALLOW ON)

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1 GIT_SHALLOW ON)

FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.3.0 GIT_SHALLOW ON)

FetchContent_Declare(vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG v1.4.352 GIT_SHALLOW ON)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0 GIT_SHALLOW ON)

# Dear ImGui — docking branch (separate from master in 2026, required for a dockable editor).
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.8-docking GIT_SHALLOW ON)

FetchContent_MakeAvailable(EnTT glm VulkanMemoryAllocator vk-bootstrap nlohmann_json imgui)

# --- Dear ImGui static lib (no upstream CMake) --------------------------------
# Core + SDL3 platform backend + Vulkan renderer backend.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp   # ImGui::InputText(std::string*)
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)
# Enable docking + viewports config flags at the API level (set in code via io.ConfigFlags).

# --- VMA implementation TU ----------------------------------------------------
# VMA is header-only; one translation unit must define VMA_IMPLEMENTATION.
add_library(vma STATIC ${CMAKE_SOURCE_DIR}/cmake/vma_impl.cpp)
target_link_libraries(vma PUBLIC GPUOpen::VulkanMemoryAllocator Vulkan::Vulkan)
target_compile_options(vma PRIVATE -Wno-nullability-completeness)

# --- stb_image_write TU -------------------------------------------------------
# Single-header, public domain (v1.16), vendored under third_party/stb. One TU
# defines STB_IMAGE_WRITE_IMPLEMENTATION; used by the renderer to write PNGs.
add_library(stb STATIC ${CMAKE_SOURCE_DIR}/cmake/stb_impl.cpp)
target_include_directories(stb PUBLIC ${CMAKE_SOURCE_DIR}/third_party/stb)
target_compile_options(stb PRIVATE -Wno-unused-function)  # stb ships static helpers

# --- Model importers ----------------------------------------------------------
# Single-header, MIT, exception-free, vendored under third_party. One impl TU
# each; the Geometry module wraps both into std::expected at the boundary.
add_library(cgltf STATIC ${CMAKE_SOURCE_DIR}/cmake/cgltf_impl.cpp)         # glTF 2.0, v1.15
target_include_directories(cgltf PUBLIC ${CMAKE_SOURCE_DIR}/third_party/cgltf)
target_compile_options(cgltf PRIVATE -Wno-unused-function)

add_library(tinyobjloader STATIC ${CMAKE_SOURCE_DIR}/cmake/tinyobjloader_impl.cpp)  # OBJ, v1.0.6
target_include_directories(tinyobjloader PUBLIC ${CMAKE_SOURCE_DIR}/third_party/tinyobjloader)

# Convenience interface target aggregating everything the engine links against.
add_library(saffron_third_party INTERFACE)
target_link_libraries(saffron_third_party INTERFACE
    SDL3::SDL3
    Vulkan::Vulkan
    EnTT::EnTT
    glm::glm
    nlohmann_json::nlohmann_json
    vk-bootstrap::vk-bootstrap
    vma
    stb
    cgltf
    tinyobjloader
    imgui)
# The engine bans exceptions; make nlohmann/json turn would-be throws into abort()
# so any stray .at()/operator[] on missing keys fails loudly instead of throwing.
# GLM_FORCE_DEPTH_ZERO_TO_ONE makes glm::perspective emit Vulkan's [0,1] clip depth.
target_compile_definitions(saffron_third_party INTERFACE JSON_NOEXCEPTION GLM_FORCE_DEPTH_ZERO_TO_ONE)
