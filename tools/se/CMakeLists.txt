add_executable(se source/main.cpp)

target_compile_features(se PRIVATE cxx_std_20)
target_include_directories(se PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(se PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(se PRIVATE JSON_NOEXCEPTION)

# se runs directly on the host (outside the saffron-build toolbox), so link
# against libstdc++ which ships with the Silverblue base OS, not libc++ which
# is toolbox-only.  Clang honours the last -stdlib= flag on the command line,
# so these options win over the global -stdlib=libc++ from the preset.
target_compile_options(se PRIVATE -stdlib=libstdc++)
target_link_options(se PRIVATE -stdlib=libstdc++)
