cmake_minimum_required(VERSION 3.20)
project(Glory VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ─────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(spdlog REQUIRED)

# ── Shader compilation ───────────────────────────────────────────────────────
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/bin)
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found – install the Vulkan SDK.")
endif()

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/shaders)

set(SHADER_SOURCES
    ${CMAKE_SOURCE_DIR}/shaders/triangle.vert
    ${CMAKE_SOURCE_DIR}/shaders/triangle.frag
    ${CMAKE_SOURCE_DIR}/shaders/grid.vert
    ${CMAKE_SOURCE_DIR}/shaders/grid.frag
    ${CMAKE_SOURCE_DIR}/shaders/debug.vert
    ${CMAKE_SOURCE_DIR}/shaders/debug.frag
    ${CMAKE_SOURCE_DIR}/shaders/skinned.vert
    ${CMAKE_SOURCE_DIR}/shaders/click_indicator.vert
    ${CMAKE_SOURCE_DIR}/shaders/click_indicator.frag
    ${CMAKE_SOURCE_DIR}/shaders/particle_sim.comp
    ${CMAKE_SOURCE_DIR}/shaders/particle.vert
    ${CMAKE_SOURCE_DIR}/shaders/particle.frag
)

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_OUTPUT ${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT  ${SPV_OUTPUT}
        COMMAND ${GLSLC} ${SHADER} -o ${SPV_OUTPUT}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    list(APPEND SPV_SHADERS ${SPV_OUTPUT})
endforeach()

add_custom_target(Shaders ALL DEPENDS ${SPV_SHADERS})

# ── meshoptimizer ─────────────────────────────────────────────────────────────
file(GLOB MESHOPT_SOURCES ${CMAKE_SOURCE_DIR}/extern/meshoptimizer/src/*.cpp)
add_library(meshoptimizer STATIC ${MESHOPT_SOURCES})
target_include_directories(meshoptimizer PUBLIC ${CMAKE_SOURCE_DIR}/extern/meshoptimizer/src)

# ── Main executable ───────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/core/Log.cpp
    src/core/Application.cpp
    src/window/Window.cpp
    src/camera/Camera.cpp
    src/input/InputManager.cpp
    src/scene/Scene.cpp
    src/renderer/Context.cpp
    src/renderer/Device.cpp
    src/renderer/Swapchain.cpp
    src/renderer/Buffer.cpp
    src/renderer/Image.cpp
    src/renderer/Mesh.cpp
    src/renderer/Model.cpp
    src/renderer/Material.cpp
    src/renderer/Texture.cpp
    src/renderer/Descriptors.cpp
    src/renderer/Pipeline.cpp
    src/renderer/Sync.cpp
    src/renderer/ClickIndicatorRenderer.cpp
    src/renderer/Renderer.cpp
    src/renderer/GLBLoader.cpp
    src/renderer/DynamicMesh.cpp
    src/renderer/StaticSkinnedMesh.cpp
    src/map/MapLoader.cpp
    src/terrain/IsometricCamera.cpp
    src/nav/DebugRenderer.cpp
    src/hud/HUD.cpp
    src/animation/AnimationPlayer.cpp
    src/animation/CPUSkinning.cpp
    src/vfx/ParticleSystem.cpp
    src/vfx/VFXRenderer.cpp
    src/ability/AbilitySystem.cpp
    extern/vma/vma_impl.cpp
    extern/imgui/imgui.cpp
    extern/imgui/imgui_draw.cpp
    extern/imgui/imgui_tables.cpp
    extern/imgui/imgui_widgets.cpp
    extern/imgui/imgui_demo.cpp
    extern/imgui/imgui_impl_glfw.cpp
    extern/imgui/imgui_impl_vulkan.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})
add_dependencies(${PROJECT_NAME} Shaders)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern/vma
    ${CMAKE_SOURCE_DIR}/extern/stb
    ${CMAKE_SOURCE_DIR}/extern/tinyobj
    ${CMAKE_SOURCE_DIR}/extern/entt
    ${CMAKE_SOURCE_DIR}/extern/imgui
    ${CMAKE_SOURCE_DIR}/extern/tinygltf
    ${CMAKE_SOURCE_DIR}/extern
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
    glfw
    glm::glm
    spdlog::spdlog
    meshoptimizer
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    GLFW_INCLUDE_VULKAN
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/"
    ASSET_DIR="${CMAKE_BINARY_DIR}/assets/"
    MAP_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/maps/"
    MODEL_DIR="${CMAKE_SOURCE_DIR}/"
)

# ── Asset copy ────────────────────────────────────────────────────────────────
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets)
file(COPY ${CMAKE_SOURCE_DIR}/assets/ DESTINATION ${CMAKE_BINARY_DIR}/assets)

# ── Map / terrain tests (lightweight, no Vulkan) ─────────────────────────────
add_executable(test_mirror tests/test_mirror.cpp)
target_include_directories(test_mirror PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mirror PRIVATE glm::glm)
target_compile_definitions(test_mirror PRIVATE GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE)

add_executable(test_maploader tests/test_maploader.cpp src/map/MapLoader.cpp)
target_include_directories(test_maploader PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/extern)
target_link_libraries(test_maploader PRIVATE glm::glm)
target_compile_definitions(test_maploader PRIVATE
    GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE
    MAP_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/maps/")

add_executable(test_terrain tests/test_terrain.cpp src/terrain/IsometricCamera.cpp)
target_include_directories(test_terrain PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_terrain PRIVATE glm::glm)
target_compile_definitions(test_terrain PRIVATE GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE)
