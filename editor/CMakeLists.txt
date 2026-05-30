add_executable(SaffronEditor)

target_sources(SaffronEditor PRIVATE source/main.cpp)

target_compile_features(SaffronEditor PRIVATE cxx_std_26)
set_target_properties(SaffronEditor PROPERTIES CXX_MODULE_STD ON)

target_link_libraries(SaffronEditor PRIVATE Saffron::Engine)
