add_executable(SaffronEditor)

target_sources(SaffronEditor PRIVATE source/main.cpp)

target_compile_features(SaffronEditor PRIVATE cxx_std_26)
set_target_properties(SaffronEditor PROPERTIES CXX_MODULE_STD ON)

target_link_libraries(SaffronEditor PRIVATE Saffron::Engine)

# Compile editor shaders (*.slang -> bin/shaders/*.spv) alongside the executable.
saffron_compile_shaders(SaffronEditor
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
    ${SAFFRON_RUNTIME_DIR}/shaders)

# Copy model assets next to the executable so assetPath("models/...") resolves.
add_custom_command(TARGET SaffronEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/models
            ${SAFFRON_RUNTIME_DIR}/models
    COMMENT "Copying models to the runtime dir")
