add_library(SaffronAnimaLib STATIC)

# Engine code is authored as C++ named modules.
target_sources(SaffronAnimaLib
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/source
        FILES
            source/saffron/core/core.cppm
            source/saffron/json/json.cppm
            source/saffron/signal/signal.cppm
            source/saffron/window/window.cppm
            source/saffron/geometry/geometry.cppm
            source/saffron/scene/scene.cppm
            source/saffron/animation/animation.cppm
            source/saffron/physics/physics_types.cppm
            source/saffron/physics/physics.cppm
            source/saffron/script/script.cppm
            source/saffron/rendering/render_graph.cppm
            source/saffron/rendering/renderer_types.cppm
            source/saffron/rendering/renderer_detail.cppm
            source/saffron/rendering/renderer.cppm
            source/saffron/assets/assets.cppm
            source/saffron/sceneedit/scene_edit_context.cppm
            source/saffron/sceneedit/sceneedit.cppm
            source/saffron/control/control_dto.cppm
            source/saffron/control/command.cppm
            source/saffron/control/control.cppm
            source/saffron/app/app.cppm
            source/saffron/host/host.cppm)

# Implementation units (module Saffron.Rendering;) are regular sources, not module
# interface units, so they live outside the CXX_MODULES file set.
target_sources(SaffronAnimaLib PRIVATE
    source/saffron/rendering/renderer_capture.cpp
    source/saffron/rendering/renderer_pipelines.cpp
    source/saffron/rendering/renderer_drawlist.cpp
    source/saffron/rendering/renderer_lighting.cpp
    source/saffron/rendering/renderer_aa.cpp
    source/saffron/rendering/renderer_textures.cpp
    source/saffron/rendering/renderer_thumbnail.cpp
    source/saffron/rendering/renderer_detail_ibl.cpp
    source/saffron/rendering/renderer_profiler.cpp
    source/saffron/scene/scene_component_serde.generated.cpp
    source/saffron/animation/animation.cpp
    source/saffron/physics/physics.cpp
    source/saffron/script/script_runtime.cpp
    source/saffron/sceneedit/scene_edit_context.cpp
    source/saffron/sceneedit/scene_edit_components.cpp
    source/saffron/sceneedit/scene_edit_camera.cpp
    source/saffron/sceneedit/scene_edit_gizmo.cpp
    source/saffron/sceneedit/scene_edit_play.cpp
    source/saffron/control/control_dto_serde.generated.cpp
    source/saffron/control/control_commands_render.cpp
    source/saffron/control/control_commands_scene.cpp
    source/saffron/control/control_commands_asset.cpp
    source/saffron/control/control_commands_animation.cpp
    source/saffron/control/control_commands_physics.cpp
    source/saffron/control/control_server.cpp
    source/saffron/assets/assets_thumbnail.cpp)

target_compile_features(SaffronAnimaLib PUBLIC cxx_std_26)
set_target_properties(SaffronAnimaLib PROPERTIES CXX_MODULE_STD ON)

target_link_libraries(SaffronAnimaLib PUBLIC saffron_third_party)

# physics.cpp is the only TU that includes <Jolt/...>; it alone needs Jolt's arch +
# thread flags (captured in Dependencies.cmake and kept off every other TU so the
# `import std` modules' POSIX-thread config stays consistent). The pimpl keeps Jolt
# types out of the module boundary, so compiling just this TU with AVX2 is ABI-safe.
set_source_files_properties(source/saffron/physics/physics.cpp
    PROPERTIES COMPILE_OPTIONS "${SAFFRON_JOLT_COMPILE_OPTIONS}")

add_library(Saffron::Anima ALIAS SaffronAnimaLib)

add_executable(SaffronAnima)

target_sources(SaffronAnima PRIVATE source/main.cpp)

target_compile_features(SaffronAnima PRIVATE cxx_std_26)
set_target_properties(SaffronAnima PROPERTIES CXX_MODULE_STD ON)

target_link_libraries(SaffronAnima PRIVATE Saffron::Anima)

# Compile engine shaders (*.slang -> bin/shaders/*.spv) alongside the executable.
saffron_compile_shaders(SaffronAnima
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
    ${SAFFRON_RUNTIME_DIR}/shaders)

# Copy model + font assets next to the executable so assetPath(...) resolves.
add_custom_command(TARGET SaffronAnima POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/models
            ${SAFFRON_RUNTIME_DIR}/models
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts
            ${SAFFRON_RUNTIME_DIR}/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons
            ${SAFFRON_RUNTIME_DIR}/icons
    COMMENT "Copying models + fonts + icons to the runtime dir")
