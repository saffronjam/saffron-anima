add_executable(sa source/main.cpp)

target_compile_features(sa PRIVATE cxx_std_20)
target_include_directories(sa PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sa PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(sa PRIVATE JSON_NOEXCEPTION)

# sa runs directly on the host (outside the saffron-build toolbox), so link
# against libstdc++ which ships with the Silverblue base OS, not libc++ which
# is toolbox-only.  Clang honours the last -stdlib= flag on the command line,
# so these options win over the global -stdlib=libc++ from the preset.
target_compile_options(sa PRIVATE -stdlib=libstdc++)
target_link_options(sa PRIVATE -stdlib=libstdc++)
