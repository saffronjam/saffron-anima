add_library(SaffronEngine STATIC)

# Engine code is authored as C++ named modules.
target_sources(SaffronEngine
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
            source/saffron/rendering/render_graph.cppm
            source/saffron/rendering/renderer_types.cppm
            source/saffron/rendering/renderer_detail.cppm
            source/saffron/rendering/renderer.cppm
            source/saffron/ui/ui.cppm
            source/saffron/assets/assets.cppm
            source/saffron/editor/editor.cppm
            source/saffron/control/control.cppm
            source/saffron/app/app.cppm)

# Implementation units (module Saffron.Rendering;) are regular sources, not module
# interface units, so they live outside the CXX_MODULES file set.
target_sources(SaffronEngine PRIVATE
    source/saffron/rendering/renderer_capture.cpp
    source/saffron/rendering/renderer_pipelines.cpp
    source/saffron/rendering/renderer_drawlist.cpp
    source/saffron/rendering/renderer_lighting.cpp
    source/saffron/rendering/renderer_aa.cpp
    source/saffron/rendering/renderer_textures.cpp
    source/saffron/rendering/renderer_thumbnail.cpp)

target_compile_features(SaffronEngine PUBLIC cxx_std_26)
set_target_properties(SaffronEngine PROPERTIES CXX_MODULE_STD ON)

target_link_libraries(SaffronEngine PUBLIC saffron_third_party)

add_library(Saffron::Engine ALIAS SaffronEngine)
