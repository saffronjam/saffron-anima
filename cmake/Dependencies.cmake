# Third-party dependencies. Libraries are vendored statically via FetchContent
# (pinned tags) except SDL3 and the Vulkan headers/loader, which come from the
# system (saffron-build toolbox). All current as of 2026-05.

include(FetchContent)

# --- System packages ----------------------------------------------------------
find_package(Vulkan REQUIRED)          # Vulkan headers + loader (we use the raw C API, not vulkan.hpp/raii)
find_package(SDL3 REQUIRED CONFIG)     # SDL3 3.4.x, C ABI
find_package(X11 REQUIRED)             # X11 child-window embedding for the native-viewport bridge

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

FetchContent_MakeAvailable(EnTT glm VulkanMemoryAllocator vk-bootstrap nlohmann_json)

# --- Lua 5.5.0 (built from source; ships no CMake) ----------------------------
# Lua's tarball provides only a Makefile, so we populate the sources and compile
# the core + stdlib into one static lib. lua.c / luac.c are the CLI front-ends
# (each has main()) and are excluded. Vendored because CMake 3.31's FindLua
# predates 5.5. Compiled as C so the error model stays setjmp/longjmp; lua_static
# is the project's only C target, so C is enabled here rather than in project().
enable_language(C)
FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.5.0.tar.gz
    URL_HASH SHA256=57ccc32bbbd005cab75bcc52444052535af691789dba2b9016d5c50640d68b3d)
FetchContent_MakeAvailable(lua)

file(GLOB lua_sources CONFIGURE_DEPENDS ${lua_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM lua_sources
    ${lua_SOURCE_DIR}/src/lua.c        # standalone interpreter main()
    ${lua_SOURCE_DIR}/src/luac.c)      # bytecode compiler main()

add_library(lua_static STATIC ${lua_sources})
target_include_directories(lua_static PUBLIC ${lua_SOURCE_DIR}/src)
# LUA_USE_POSIX (not LUA_USE_LINUX) avoids the dl/readline pull-in that only the
# excluded lua.c front-end needs.
target_compile_definitions(lua_static PRIVATE LUA_USE_POSIX)
set_target_properties(lua_static PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)

# --- LuaBridge3 (header-only C++ <-> Lua bindings; supports Lua 5.5) ----------
# SYSTEM keeps its headers out of our warning set (Expected.h trips
# -Wdeprecated-declarations under C++26).
FetchContent_Declare(LuaBridge3
    GIT_REPOSITORY https://github.com/kunitoki/LuaBridge3.git
    GIT_TAG 3.0-rc12 GIT_SHALLOW ON         # pin a specific rc, do not track master
    SYSTEM)
FetchContent_MakeAvailable(LuaBridge3)

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

# --- nanosvg TU ---------------------------------------------------------------
# Single-header, zlib license, vendored under third_party/nanosvg. One impl TU;
# the renderer rasterizes SVG asset icons to GPU textures via uploadSvgIcon.
add_library(nanosvg STATIC ${CMAKE_SOURCE_DIR}/cmake/nanosvg_impl.cpp)
target_include_directories(nanosvg PUBLIC ${CMAKE_SOURCE_DIR}/third_party/nanosvg)
target_compile_options(nanosvg PRIVATE -Wno-unused-function)

# Convenience interface target aggregating everything the engine links against.
add_library(saffron_third_party INTERFACE)
target_link_libraries(saffron_third_party INTERFACE
    SDL3::SDL3
    X11::X11
    Vulkan::Vulkan
    EnTT::EnTT
    glm::glm
    nlohmann_json::nlohmann_json
    vk-bootstrap::vk-bootstrap
    vma
    stb
    cgltf
    tinyobjloader
    nanosvg
    lua_static
    LuaBridge)
# The engine bans exceptions; make nlohmann/json turn would-be throws into abort()
# so any stray .at()/operator[] on missing keys fails loudly instead of throwing.
# GLM_FORCE_DEPTH_ZERO_TO_ONE makes glm::perspective emit Vulkan's [0,1] clip depth.
target_compile_definitions(saffron_third_party INTERFACE
    JSON_NOEXCEPTION GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL)
