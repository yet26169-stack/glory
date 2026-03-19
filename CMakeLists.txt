cmake_minimum_required(VERSION 3.20)
project(Glory VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# ── Sanitizer support ─────────────────────────────────────────────────────────
# Usage: cmake -B build -DGLORY_ASAN=ON -DGLORY_UBSAN=ON
#         cmake -B build -DGLORY_TSAN=ON   (ASAN and TSAN are mutually exclusive)
option(GLORY_ASAN  "Enable AddressSanitizer"          OFF)
option(GLORY_TSAN  "Enable ThreadSanitizer"            OFF)
option(GLORY_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(GLORY_ASAN AND GLORY_TSAN)
    message(FATAL_ERROR "GLORY_ASAN and GLORY_TSAN are mutually exclusive — enable only one.")
endif()

add_library(glory_sanitizers INTERFACE)
if(GLORY_ASAN)
    if(MSVC)
        target_compile_options(glory_sanitizers INTERFACE /fsanitize=address /Zi)
        target_link_options(glory_sanitizers INTERFACE /fsanitize=address /DEBUG)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(glory_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options   (glory_sanitizers INTERFACE -fsanitize=address)
    endif()
    message(STATUS "Glory: AddressSanitizer ENABLED")
endif()
if(GLORY_TSAN)
    if(MSVC)
        message(WARNING "ThreadSanitizer (TSAN) is not supported on MSVC — disabling.")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(glory_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer -g)
        target_link_options   (glory_sanitizers INTERFACE -fsanitize=thread)
        message(STATUS "Glory: ThreadSanitizer ENABLED")
    endif()
endif()
if(GLORY_UBSAN)
    if(MSVC)
        message(WARNING "UndefinedBehaviorSanitizer (UBSAN) is not supported on MSVC — disabling.")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(glory_sanitizers INTERFACE -fsanitize=undefined -fno-omit-frame-pointer -g)
        target_link_options   (glory_sanitizers INTERFACE -fsanitize=undefined)
        message(STATUS "Glory: UndefinedBehaviorSanitizer ENABLED")
    endif()
endif()

enable_testing()

# ── System dependencies ───────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(spdlog REQUIRED)

# ── Shader compilation ────────────────────────────────────────────────────────
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/bin)
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found – install the Vulkan SDK.")
endif()

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/shaders)

file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/shaders/*.vert
    ${CMAKE_SOURCE_DIR}/shaders/*.frag
    ${CMAKE_SOURCE_DIR}/shaders/*.comp
)

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_OUTPUT ${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT  ${SPV_OUTPUT}
        COMMAND ${GLSLC} --target-env=vulkan1.3 ${SHADER} -o ${SPV_OUTPUT}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    list(APPEND SPV_SHADERS ${SPV_OUTPUT})
endforeach()

add_custom_target(Shaders ALL DEPENDS ${SPV_SHADERS})

# ── Tracy profiler (optional, default OFF) ────────────────────────────────────
# Enable with:  cmake -DGLORY_TRACY=ON ..
option(GLORY_TRACY "Enable Tracy real-time profiler instrumentation" OFF)
if(GLORY_TRACY)
    FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG        v0.11.1
        GIT_SHALLOW    TRUE
    )
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(tracy)
    message(STATUS "Glory: Tracy profiler ENABLED (v0.11.1)")
else()
    message(STATUS "Glory: Tracy profiler disabled  (pass -DGLORY_TRACY=ON to enable)")
endif()

# ── ENet networking library ───────────────────────────────────────────────────
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(enet)
target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
message(STATUS "Glory: ENet networking library ENABLED (v1.3.18)")

# ── Lua 5.4 ──────────────────────────────────────────────────────────────────
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.7
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(lua)

file(GLOB LUA_CORE_SOURCES ${lua_SOURCE_DIR}/*.c)
list(FILTER LUA_CORE_SOURCES EXCLUDE REGEX "lua\\.c$")
list(FILTER LUA_CORE_SOURCES EXCLUDE REGEX "luac\\.c$")
add_library(lua_static STATIC ${LUA_CORE_SOURCES})
target_include_directories(lua_static PUBLIC ${lua_SOURCE_DIR})
set_target_properties(lua_static PROPERTIES LINKER_LANGUAGE C)
if(APPLE OR UNIX)
    target_compile_definitions(lua_static PRIVATE LUA_USE_POSIX)
endif()
message(STATUS "Glory: Lua 5.4 ENABLED (built from source)")

# ── sol2 (header-only Lua binding) ───────────────────────────────────────────
FetchContent_Declare(
    sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG        develop
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(sol2)
message(STATUS "Glory: sol2 v3.3.1 ENABLED")

# ── RecastNavigation ──────────────────────────────────────────────────────────
set(CMAKE_POLICY_DEFAULT_CMP0000 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(
    recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG        v1.6.0
    GIT_SHALLOW    TRUE
)
set(RECASTNAVIGATION_DEMO     OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS    OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_STATIC   ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(recastnavigation)
message(STATUS "Glory: RecastNavigation ENABLED (v1.6.0)")

# ── miniaudio (single-header audio) ──────────────────────────────────────────
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.21
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(miniaudio)
message(STATUS "Glory: miniaudio ENABLED (0.11.21)")

# ── meshoptimizer ─────────────────────────────────────────────────────────────
file(GLOB MESHOPT_SOURCES ${CMAKE_SOURCE_DIR}/extern/meshoptimizer/src/*.cpp)
add_library(meshoptimizer STATIC ${MESHOPT_SOURCES})
target_include_directories(meshoptimizer PUBLIC ${CMAKE_SOURCE_DIR}/extern/meshoptimizer/src)

# ── Engine static library ─────────────────────────────────────────────────────
# Collects every .cpp under src/ except main.cpp, plus the vendored extern
# implementations.  Add or delete source files freely — no list to maintain.
file(GLOB_RECURSE ENGINE_SRCS CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)
list(FILTER ENGINE_SRCS EXCLUDE REGEX ".*/main\\.cpp$")

add_library(glory_engine STATIC
    ${ENGINE_SRCS}
    extern/vma/vma_impl.cpp
    extern/imgui/imgui.cpp
    extern/imgui/imgui_draw.cpp
    extern/imgui/imgui_tables.cpp
    extern/imgui/imgui_widgets.cpp
    extern/imgui/imgui_demo.cpp
    extern/imgui/imgui_impl_glfw.cpp
    extern/imgui/imgui_impl_vulkan.cpp
)

target_include_directories(glory_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern/vma
    ${CMAKE_SOURCE_DIR}/extern/stb
    ${CMAKE_SOURCE_DIR}/extern/tinyobj
    ${CMAKE_SOURCE_DIR}/extern/entt
    ${CMAKE_SOURCE_DIR}/extern/imgui
    ${CMAKE_SOURCE_DIR}/extern/tinygltf
    ${CMAKE_SOURCE_DIR}/extern/nlohmann
    ${CMAKE_SOURCE_DIR}/extern
    ${miniaudio_SOURCE_DIR}
    ${recastnavigation_SOURCE_DIR}/Recast/Include
    ${recastnavigation_SOURCE_DIR}/Detour/Include
    ${recastnavigation_SOURCE_DIR}/DetourCrowd/Include
    ${recastnavigation_SOURCE_DIR}/DetourTileCache/Include
)

target_compile_definitions(glory_engine PUBLIC
    GLFW_INCLUDE_VULKAN
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    MAP_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/maps/"
    SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/"
    ASSET_DIR="${CMAKE_BINARY_DIR}/assets/"
    MODEL_DIR="${CMAKE_SOURCE_DIR}/"
    SOL_ALL_SAFETIES_ON=1
    SOL_USE_CXX_OPTIONAL=1
    SOL_STD_VARIANT=1
)

target_link_libraries(glory_engine PUBLIC
    Vulkan::Vulkan
    glfw
    glm::glm
    spdlog::spdlog
    meshoptimizer
    enet
    lua_static
    sol2
    glory_sanitizers
    Recast
    Detour
    DetourCrowd
    DetourTileCache
)

if(WIN32)
    target_link_libraries(glory_engine PUBLIC winmm)
endif()

if(APPLE)
    find_library(COREAUDIO_LIB     CoreAudio)
    find_library(AUDIOTOOLBOX_LIB  AudioToolbox)
    find_library(COREFOUNDATION_LIB CoreFoundation)
    target_link_libraries(glory_engine PUBLIC
        ${COREAUDIO_LIB} ${AUDIOTOOLBOX_LIB} ${COREFOUNDATION_LIB})
endif()

if(GLORY_TRACY)
    target_link_libraries(glory_engine PUBLIC TracyClient)
    target_compile_definitions(glory_engine PUBLIC TRACY_ENABLE)
endif()

# ── Asset copy ────────────────────────────────────────────────────────────────
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets)
file(COPY ${CMAKE_SOURCE_DIR}/assets/ DESTINATION ${CMAKE_BINARY_DIR}/assets)

add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        ${CMAKE_BINARY_DIR}/assets
    COMMENT "Syncing assets to build directory"
)

# ── Main executable ───────────────────────────────────────────────────────────
set(COOKED_ASSET_DIR ${CMAKE_BINARY_DIR}/assets_cooked)

add_executable(Glory src/main.cpp)
add_dependencies(Glory Shaders copy_assets)
target_link_libraries(Glory PRIVATE glory_engine)
target_compile_definitions(Glory PRIVATE
    COOKED_ASSET_DIR="${COOKED_ASSET_DIR}/"
)

# ── Test infrastructure ───────────────────────────────────────────────────────
# glory_test_common is a named interface through which all tests inherit the
# engine's include paths, compile definitions, and link libraries.  Adding a
# new engine-wide test dependency means touching only this one target.
add_library(glory_test_common INTERFACE)
target_link_libraries(glory_test_common INTERFACE glory_engine)

# ── Test executables ──────────────────────────────────────────────────────────
add_executable(test_mirror     tests/test_mirror.cpp)
add_executable(test_maploader  tests/test_maploader.cpp)
add_executable(test_terrain    tests/test_terrain.cpp)
add_executable(test_fixedpoint tests/test_fixedpoint.cpp)
add_executable(test_nav        tests/test_nav.cpp)
add_executable(test_ability    tests/test_ability.cpp)
add_executable(test_combat     tests/test_combat.cpp)
add_executable(test_animation  tests/test_animation.cpp)
add_executable(test_physics    tests/test_physics.cpp)
add_executable(test_audio      tests/test_audio.cpp)
add_executable(test_replay     tests/test_replay.cpp)

foreach(_test
    test_mirror test_maploader test_terrain test_fixedpoint
    test_nav test_ability test_combat test_animation
    test_physics test_audio test_replay)
    target_link_libraries(${_test} PRIVATE glory_test_common)
endforeach()

add_test(NAME fixedpoint COMMAND test_fixedpoint)
add_test(NAME mirror     COMMAND test_mirror)
add_test(NAME maploader  COMMAND test_maploader)
add_test(NAME terrain    COMMAND test_terrain)
add_test(NAME nav        COMMAND test_nav)
add_test(NAME ability    COMMAND test_ability)
add_test(NAME combat     COMMAND test_combat)
add_test(NAME animation  COMMAND test_animation)
add_test(NAME physics    COMMAND test_physics)
add_test(NAME audio      COMMAND test_audio)
add_test(NAME replay     COMMAND test_replay)

# ── Asset cook tool ───────────────────────────────────────────────────────────
add_executable(asset_cook tools/asset_cook.cpp)
target_include_directories(asset_cook PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern/tinygltf
    ${CMAKE_SOURCE_DIR}/extern/stb
    ${CMAKE_SOURCE_DIR}/extern/nlohmann
    ${CMAKE_SOURCE_DIR}/extern
)
target_link_libraries(asset_cook PRIVATE meshoptimizer)
target_compile_definitions(asset_cook PRIVATE GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE)

# ── Cook assets target ────────────────────────────────────────────────────────
add_custom_target(cook_assets
    COMMAND ${CMAKE_COMMAND} -E make_directory ${COOKED_ASSET_DIR}
    COMMAND asset_cook ${CMAKE_SOURCE_DIR}/assets ${COOKED_ASSET_DIR}
    DEPENDS asset_cook
    COMMENT "Cooking assets..."
)
