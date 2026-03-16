cmake_minimum_required(VERSION 3.20)
project(Glory VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
    target_compile_options(glory_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options   (glory_sanitizers INTERFACE -fsanitize=address)
    message(STATUS "Glory: AddressSanitizer ENABLED")
endif()
if(GLORY_TSAN)
    target_compile_options(glory_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options   (glory_sanitizers INTERFACE -fsanitize=thread)
    message(STATUS "Glory: ThreadSanitizer ENABLED")
endif()
if(GLORY_UBSAN)
    target_compile_options(glory_sanitizers INTERFACE -fsanitize=undefined -fno-omit-frame-pointer -g)
    target_link_options   (glory_sanitizers INTERFACE -fsanitize=undefined)
    message(STATUS "Glory: UndefinedBehaviorSanitizer ENABLED")
endif()

enable_testing()

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
    ${CMAKE_SOURCE_DIR}/shaders/shield_bubble.vert
    ${CMAKE_SOURCE_DIR}/shaders/shield_bubble.frag
    ${CMAKE_SOURCE_DIR}/shaders/cone_ability.vert
    ${CMAKE_SOURCE_DIR}/shaders/cone_grid.frag
    ${CMAKE_SOURCE_DIR}/shaders/cone_energy.frag
    ${CMAKE_SOURCE_DIR}/shaders/cone_lightning.vert
    ${CMAKE_SOURCE_DIR}/shaders/cone_lightning.frag
    ${CMAKE_SOURCE_DIR}/shaders/explosion_disk.vert
    ${CMAKE_SOURCE_DIR}/shaders/explosion_sphere.vert
    ${CMAKE_SOURCE_DIR}/shaders/explosion_shockwave.frag
    ${CMAKE_SOURCE_DIR}/shaders/explosion_fireball.frag
    ${CMAKE_SOURCE_DIR}/shaders/sprite_effect_additive.frag
    ${CMAKE_SOURCE_DIR}/shaders/sprite_effect_billboard.vert
    ${CMAKE_SOURCE_DIR}/shaders/bloom_extract.frag
    ${CMAKE_SOURCE_DIR}/shaders/bloom_blur.frag
    ${CMAKE_SOURCE_DIR}/shaders/tonemap.vert
    ${CMAKE_SOURCE_DIR}/shaders/tonemap.frag
    ${CMAKE_SOURCE_DIR}/shaders/trail_ribbon.vert
    ${CMAKE_SOURCE_DIR}/shaders/trail_ribbon.frag
    ${CMAKE_SOURCE_DIR}/shaders/ground_decal.vert
    ${CMAKE_SOURCE_DIR}/shaders/ground_decal.frag
    ${CMAKE_SOURCE_DIR}/shaders/mesh_effect.vert
    ${CMAKE_SOURCE_DIR}/shaders/mesh_effect_energy.frag
    ${CMAKE_SOURCE_DIR}/shaders/mesh_effect_slash.frag
    ${CMAKE_SOURCE_DIR}/shaders/distortion.frag
    ${CMAKE_SOURCE_DIR}/shaders/inking.vert
    ${CMAKE_SOURCE_DIR}/shaders/inking.frag
    ${CMAKE_SOURCE_DIR}/shaders/fow_upsample.comp
    ${CMAKE_SOURCE_DIR}/shaders/fow_blur.comp
    ${CMAKE_SOURCE_DIR}/shaders/outline_stencil.frag
    ${CMAKE_SOURCE_DIR}/shaders/outline.vert
    ${CMAKE_SOURCE_DIR}/shaders/outline.frag
    ${CMAKE_SOURCE_DIR}/shaders/water.vert
    ${CMAKE_SOURCE_DIR}/shaders/water.frag
    ${CMAKE_SOURCE_DIR}/shaders/particle_compact.comp
    ${CMAKE_SOURCE_DIR}/shaders/particle_sort.comp
    ${CMAKE_SOURCE_DIR}/shaders/shadow.vert
    ${CMAKE_SOURCE_DIR}/shaders/shadow_skinned.vert
    ${CMAKE_SOURCE_DIR}/shaders/depth_only.vert
    ${CMAKE_SOURCE_DIR}/shaders/hiz_generate.comp
    ${CMAKE_SOURCE_DIR}/shaders/occlusion_cull.comp
    ${CMAKE_SOURCE_DIR}/shaders/spatial_hash.comp
    ${CMAKE_SOURCE_DIR}/shaders/collision_broadphase.comp
    ${CMAKE_SOURCE_DIR}/shaders/ssao.comp
    ${CMAKE_SOURCE_DIR}/shaders/ssao_blur.comp
    ${CMAKE_SOURCE_DIR}/shaders/ssr.comp
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
# Requires internet access on first configure to fetch the Tracy source.
option(GLORY_TRACY "Enable Tracy real-time profiler instrumentation" OFF)
if(GLORY_TRACY)
    include(FetchContent)
    FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG        v0.11.1
        GIT_SHALLOW    TRUE
    )
    # Tell Tracy's own CMakeLists to compile the client with profiling on.
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(tracy)
    message(STATUS "Glory: Tracy profiler ENABLED (v0.11.1)")
else()
    message(STATUS "Glory: Tracy profiler disabled  (pass -DGLORY_TRACY=ON to enable)")
endif()

# ── meshoptimizer ─────────────────────────────────────────────────────────────
file(GLOB MESHOPT_SOURCES ${CMAKE_SOURCE_DIR}/extern/meshoptimizer/src/*.cpp)
add_library(meshoptimizer STATIC ${MESHOPT_SOURCES})
target_include_directories(meshoptimizer PUBLIC ${CMAKE_SOURCE_DIR}/extern/meshoptimizer/src)

# ── Main executable ───────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/core/Log.cpp
    src/core/Application.cpp
    src/core/FramePacer.cpp
    src/core/SimulationLoop.cpp
    src/core/SystemScheduler.cpp
    src/core/GameSystems.cpp
    src/physics/PhysicsSystem.cpp
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
    src/renderer/BindlessDescriptors.cpp
    src/renderer/Pipeline.cpp
    src/renderer/Sync.cpp
    src/renderer/ClickIndicatorRenderer.cpp
    src/renderer/ShieldBubbleRenderer.cpp
    src/renderer/ConeAbilityRenderer.cpp
    src/renderer/ExplosionRenderer.cpp
    src/renderer/SpriteEffectRenderer.cpp
    src/renderer/Renderer.cpp
    src/renderer/GpuTimer.cpp
    src/renderer/GLBLoader.cpp
    src/renderer/DynamicMesh.cpp
    src/renderer/StaticSkinnedMesh.cpp
    src/renderer/HDRFramebuffer.cpp
    src/renderer/ShadowPass.cpp
    src/renderer/HiZPass.cpp
    src/renderer/MegaBuffer.cpp
    src/renderer/AsyncComputeManager.cpp
    src/renderer/BloomPass.cpp
    src/renderer/ToneMapPass.cpp
    src/renderer/SSAOPass.cpp
    src/renderer/SSRPass.cpp
    src/renderer/GroundDecalRenderer.cpp
    src/renderer/DistortionRenderer.cpp
    src/renderer/InkingPass.cpp
    src/renderer/OutlineRenderer.cpp
    src/renderer/WaterRenderer.cpp
    src/renderer/RenderGraph.cpp
    src/renderer/passes/PassSetup.cpp
    src/fog/FogSystem.cpp
    src/renderer/FogOfWarRenderer.cpp
    src/map/MapLoader.cpp
    src/terrain/IsometricCamera.cpp
    src/nav/DebugRenderer.cpp
    src/nav/NavMeshBuilder.cpp
    src/nav/PathfindingSystem.cpp
    src/nav/DynamicObstacle.cpp
    src/hud/HUD.cpp
    src/hud/Minimap.cpp
    src/hud/PerfOverlay.cpp
    src/animation/AnimationPlayer.cpp
    src/animation/CPUSkinning.cpp
    src/vfx/ParticleSystem.cpp
    src/vfx/VFXRenderer.cpp
    src/vfx/VFXDefinitionLoader.cpp
    src/vfx/VFXFactory.cpp
    src/vfx/TrailRenderer.cpp
    src/vfx/MeshEffectRenderer.cpp
    src/vfx/CompositeVFXSequencer.cpp
    src/ability/AbilitySystem.cpp
    src/ability/ProjectileSystem.cpp
    src/combat/CombatSystem.cpp
    src/combat/GpuCollisionSystem.cpp
    src/audio/AudioEngine.cpp
    src/audio/SoundHandle.cpp
    src/audio/AudioResourceManager.cpp
    src/audio/GameAudioEvents.cpp
    src/assets/CookedLoader.cpp
    src/assets/AssetRegistry.cpp
    src/scripting/ScriptEngine.cpp
    src/scripting/LuaBindings.cpp
    src/network/Transport.cpp
    src/network/InputSync.cpp
    src/network/StateSnapshot.cpp
    src/network/NetworkGameLoop.cpp
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
    glory_sanitizers
)

# Platform-specific libs for FramePacer
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE winmm)
endif()

# Tracy client (only when GLORY_TRACY=ON)
if(GLORY_TRACY)
    target_link_libraries(${PROJECT_NAME} PRIVATE TracyClient)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TRACY_ENABLE)
endif()

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
# Configure-time copy (ensures assets exist on first build)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets)
file(COPY ${CMAKE_SOURCE_DIR}/assets/ DESTINATION ${CMAKE_BINARY_DIR}/assets)

# Build-time copy (keeps build/assets in sync with source/assets on every build)
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        ${CMAKE_BINARY_DIR}/assets
    COMMENT "Syncing assets to build directory"
)
add_dependencies(${PROJECT_NAME} copy_assets)

# ── Map / terrain tests (lightweight, no Vulkan) ─────────────────────────────
add_executable(test_mirror tests/test_mirror.cpp)
target_include_directories(test_mirror PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_mirror PRIVATE glm::glm glory_sanitizers)
target_compile_definitions(test_mirror PRIVATE GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE)

add_executable(test_maploader tests/test_maploader.cpp src/map/MapLoader.cpp)
target_include_directories(test_maploader PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/extern)
target_link_libraries(test_maploader PRIVATE glm::glm glory_sanitizers)
target_compile_definitions(test_maploader PRIVATE
    GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE
    MAP_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/maps/")

add_executable(test_terrain tests/test_terrain.cpp src/terrain/IsometricCamera.cpp)
target_include_directories(test_terrain PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_terrain PRIVATE glm::glm glory_sanitizers)
target_compile_definitions(test_terrain PRIVATE GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE)

add_test(NAME mirror    COMMAND test_mirror)
add_test(NAME maploader COMMAND test_maploader)
add_test(NAME terrain   COMMAND test_terrain)

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
