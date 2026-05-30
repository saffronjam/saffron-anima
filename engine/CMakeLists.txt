add_library(SaffronEngine STATIC)

# Engine code is authored as C++ named modules.
target_sources(SaffronEngine
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/source
        FILES
            source/saffron/core/core.cppm
            source/saffron/signal/signal.cppm
            source/saffron/window/window.cppm
            source/saffron/geometry/geometry.cppm
            source/saffron/scene/scene.cppm
            source/saffron/rendering/renderer.cppm
            source/saffron/ui/ui.cppm
            source/saffron/editor/editor.cppm
            source/saffron/control/control.cppm
            source/saffron/app/app.cppm)

target_compile_features(SaffronEngine PUBLIC cxx_std_26)
set_target_properties(SaffronEngine PROPERTIES CXX_MODULE_STD ON)

target_link_libraries(SaffronEngine PUBLIC saffron_third_party)

add_library(Saffron::Engine ALIAS SaffronEngine)
