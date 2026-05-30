cmake_minimum_required(VERSION 3.31)

# Enable CMake's experimental `import std;` support (UUID is specific to CMake 3.31.x).
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")

project(SaffronEngine
    VERSION 0.1.0
    DESCRIPTION "A from-scratch Vulkan renderer / C++26 game engine"
    LANGUAGES CXX)

# --- Language standard --------------------------------------------------------
# C++26 with modules + `import std`. Extensions are left ON (gnu++26): CMake builds
# the internal std module as gnu++26, so consumers must match or the BMI is rejected.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Single output location for the runnable editor + its assets/shaders.
set(SAFFRON_RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SAFFRON_RUNTIME_DIR}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Dependencies)

add_subdirectory(engine)
add_subdirectory(editor)
