cmake_minimum_required(VERSION 3.20)
project(Glory VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Deterministic simulation option ──────────────────────────────────────────
option(GLORY_DETERMINISTIC "Use fixed-point math for deterministic simulation" OFF)

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
    ${CMAKE_SOURCE_DIR}/shaders/gbuffer.vert
    ${CMAKE_SOURCE_DIR}/shaders/gbuffer.frag
    ${CMAKE_SOURCE_DIR}/shaders/deferred.vert
    ${CMAKE_SOURCE_DIR}/shaders/deferred.frag
    ${CMAKE_SOURCE_DIR}/shaders/shadow.vert
    ${CMAKE_SOURCE_DIR}/shaders/shadow.frag
    ${CMAKE_SOURCE_DIR}/shaders/postprocess.vert
    ${CMAKE_SOURCE_DIR}/shaders/postprocess.frag
    ${CMAKE_SOURCE_DIR}/shaders/sky.frag
    ${CMAKE_SOURCE_DIR}/shaders/bloom_extract.frag
    ${CMAKE_SOURCE_DIR}/shaders/bloom_blur.frag
    ${CMAKE_SOURCE_DIR}/shaders/grid.vert
    ${CMAKE_SOURCE_DIR}/shaders/grid.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssao.frag
    ${CMAKE_SOURCE_DIR}/shaders/ssao_blur.frag
    ${CMAKE_SOURCE_DIR}/shaders/particle.vert
    ${CMAKE_SOURCE_DIR}/shaders/particle.frag
    ${CMAKE_SOURCE_DIR}/shaders/particle_sim.comp
    ${CMAKE_SOURCE_DIR}/shaders/terrain.vert
    ${CMAKE_SOURCE_DIR}/shaders/terrain.frag
    ${CMAKE_SOURCE_DIR}/shaders/water.vert
    ${CMAKE_SOURCE_DIR}/shaders/water.frag
    ${CMAKE_SOURCE_DIR}/shaders/debug.vert
    ${CMAKE_SOURCE_DIR}/shaders/debug.frag
    ${CMAKE_SOURCE_DIR}/shaders/fog.vert
    ${CMAKE_SOURCE_DIR}/shaders/fog.frag
    ${CMAKE_SOURCE_DIR}/shaders/skinned.vert
    ${CMAKE_SOURCE_DIR}/shaders/shadow_skinned.vert
    ${CMAKE_SOURCE_DIR}/shaders/cull.comp
    ${CMAKE_SOURCE_DIR}/shaders/skinning.comp
    ${CMAKE_SOURCE_DIR}/shaders/click_indicator.vert
    ${CMAKE_SOURCE_DIR}/shaders/click_indicator.frag
    # AAA plan shaders
    ${CMAKE_SOURCE_DIR}/shaders/fow_visibility.comp
    ${CMAKE_SOURCE_DIR}/shaders/fow_blur.comp
    ${CMAKE_SOURCE_DIR}/shaders/hiz_generate.comp
    ${CMAKE_SOURCE_DIR}/shaders/hiz_cull.comp
    ${CMAKE_SOURCE_DIR}/shaders/flow_field_integration.comp
    ${CMAKE_SOURCE_DIR}/shaders/visbuf_resolve.comp
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

# ── Executable ───────────────────────────────────────────────────────────────
# ── meshoptimizer (mesh simplification) ──────────────────────────────────────
file(GLOB MESHOPT_SOURCES ${CMAKE_SOURCE_DIR}/extern/meshoptimizer/src/*.cpp)
add_library(meshoptimizer STATIC ${MESHOPT_SOURCES})
target_include_directories(meshoptimizer PUBLIC ${CMAKE_SOURCE_DIR}/extern/meshoptimizer/src)

set(SOURCES
    src/main.cpp
    src/core/Log.cpp
    src/core/Application.cpp
    src/core/Profiler.cpp
    src/window/Window.cpp
    src/camera/Camera.cpp
    src/input/InputManager.cpp
    src/audio/AudioEngine.cpp
    src/physics/PhysicsEngine.cpp
    src/editor/DebugOverlay.cpp
    src/scene/Scene.cpp
    src/renderer/Context.cpp
    src/renderer/Device.cpp
    src/renderer/Swapchain.cpp
    src/renderer/Buffer.cpp
    src/renderer/Image.cpp
    src/renderer/Mesh.cpp
    src/renderer/Model.cpp
    src/renderer/Material.cpp
    src/renderer/GBuffer.cpp
    src/renderer/ShadowMap.cpp
    src/renderer/PostProcess.cpp
    src/renderer/Bloom.cpp
    src/renderer/SSAO.cpp
    src/renderer/ParticleSystem.cpp
    src/renderer/Texture.cpp
    src/renderer/Descriptors.cpp
    src/renderer/Pipeline.cpp
    src/renderer/Sync.cpp
    src/renderer/ClickIndicatorRenderer.cpp
    src/renderer/Renderer.cpp
    src/map/MapLoader.cpp
    src/terrain/TerrainSystem.cpp
    src/terrain/TerrainTextures.cpp
    src/terrain/IsometricCamera.cpp
    src/nav/DebugRenderer.cpp
    src/fog/FogSystem.cpp
    src/hud/HUD.cpp
    src/hud/MinionHealthBars.cpp
    src/input/TargetingSystem.cpp
    src/combat/AutoAttackSystem.cpp
    src/ability/AbilityDef.cpp
    src/ability/AbilitySystem.cpp
    src/ability/CooldownSystem.cpp
    src/ability/EffectSystem.cpp
    src/ability/StatusEffectSystem.cpp
    src/ability/ProjectileSystem.cpp
    src/structure/StructureConfig.cpp
    src/structure/StructureSystem.cpp
    src/jungle/JungleConfig.cpp
    src/jungle/JungleSystem.cpp
    src/renderer/GLBLoader.cpp
    src/renderer/DynamicMesh.cpp
    src/renderer/StaticSkinnedMesh.cpp
    src/renderer/GpuProfiler.cpp
    src/renderer/GpuCuller.cpp
    src/renderer/CascadeShadow.cpp
    src/renderer/TextureStreamer.cpp
    src/renderer/ComputeSkinner.cpp
    src/animation/AnimationPlayer.cpp
    src/animation/CPUSkinning.cpp
    src/minion/MinionConfig.cpp
    src/minion/MinionSystem.cpp
    # AAA plan sources
    src/math/FixedPoint.cpp
    src/math/DetRNG.cpp
    src/math/MurmurHash3.cpp
    src/core/SimulationLoop.cpp
    src/core/SimToRenderSyncSystem.cpp
    src/core/StateChecksumSystem.cpp
    src/core/StateSnapshot.cpp
    src/core/WorkStealQueue.cpp
    src/net/Transport.cpp
    src/net/GameServer.cpp
    src/net/GameClient.cpp
    src/nav/FlowField.cpp
    src/nav/HierarchicalFlowField.cpp
    src/nav/SteeringSystem.cpp
    src/fog/FogReadback.cpp
    src/renderer/HiZBuffer.cpp
    src/renderer/BindlessAllocator.cpp
    src/renderer/PipelineLibrary.cpp
    src/renderer/QueueManager.cpp
    src/asset/CookedLoader.cpp
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
    ABILITY_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/abilities/"
    MODEL_DIR="${CMAKE_SOURCE_DIR}/"
    $<$<BOOL:${GLORY_DETERMINISTIC}>:GLORY_DETERMINISTIC>
)

# ── Mesh cook tool (OBJ → binary .gmesh) ────────────────────────────────────
add_executable(mesh_cook tools/mesh_cook.cpp)
target_include_directories(mesh_cook PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/tinyobj
)
target_link_libraries(mesh_cook PRIVATE glm::glm)
target_compile_definitions(mesh_cook PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

# ── Asset cooking (build-time OBJ → .gmesh conversion) ──────────────────────
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/assets)

file(GLOB OBJ_ASSETS ${CMAKE_SOURCE_DIR}/assets/*.obj)
set(GMESH_OUTPUTS "")
foreach(OBJ_FILE ${OBJ_ASSETS})
    get_filename_component(OBJ_NAME ${OBJ_FILE} NAME_WE)
    set(GMESH_OUTPUT ${CMAKE_BINARY_DIR}/assets/${OBJ_NAME}.gmesh)
    add_custom_command(
        OUTPUT  ${GMESH_OUTPUT}
        COMMAND mesh_cook ${OBJ_FILE} ${GMESH_OUTPUT}
        DEPENDS mesh_cook ${OBJ_FILE}
        COMMENT "Cooking asset ${OBJ_NAME}.obj -> ${OBJ_NAME}.gmesh"
    )
    list(APPEND GMESH_OUTPUTS ${GMESH_OUTPUT})
endforeach()

add_custom_target(Assets ALL DEPENDS ${GMESH_OUTPUTS})
add_dependencies(${PROJECT_NAME} Assets)

# ── Map System Tests ─────────────────────────────────────────────────────────
add_executable(test_mirror tests/test_mirror.cpp)
target_include_directories(test_mirror PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_mirror PRIVATE glm::glm)
target_compile_definitions(test_mirror PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

add_executable(test_maploader tests/test_maploader.cpp src/map/MapLoader.cpp)
target_include_directories(test_maploader PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern
)
target_link_libraries(test_maploader PRIVATE glm::glm)
target_compile_definitions(test_maploader PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    MAP_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/maps/"
)

add_executable(test_terrain tests/test_terrain.cpp
    src/terrain/IsometricCamera.cpp)
target_include_directories(test_terrain PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_terrain PRIVATE glm::glm)
target_compile_definitions(test_terrain PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

add_executable(test_nav tests/test_nav.cpp src/map/MapLoader.cpp)
target_include_directories(test_nav PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern
)
target_link_libraries(test_nav PRIVATE glm::glm)
target_compile_definitions(test_nav PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    MAP_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/maps/"
)

add_executable(test_fog tests/test_fog.cpp src/fog/FogSystem.cpp src/renderer/Buffer.cpp extern/vma/vma_impl.cpp)
target_include_directories(test_fog PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern/vma
    ${CMAKE_SOURCE_DIR}/extern
)
target_link_libraries(test_fog PRIVATE glm::glm Vulkan::Vulkan spdlog::spdlog)
target_compile_definitions(test_fog PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    SHADER_DIR="${CMAKE_BINARY_DIR}/shaders/"
)

add_executable(test_ability tests/test_ability.cpp
    src/ability/AbilityDef.cpp
    src/ability/AbilitySystem.cpp
    src/ability/CooldownSystem.cpp
    src/ability/EffectSystem.cpp
    src/ability/StatusEffectSystem.cpp
)
target_include_directories(test_ability PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern
    ${CMAKE_SOURCE_DIR}/extern/entt
    ${CMAKE_SOURCE_DIR}/extern/vma
)
target_link_libraries(test_ability PRIVATE glm::glm spdlog::spdlog)
target_compile_definitions(test_ability PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    ABILITY_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/abilities/"
)

add_executable(test_minion tests/test_minion.cpp
    src/minion/MinionSystem.cpp
    src/minion/MinionConfig.cpp
    src/map/MapLoader.cpp
)
target_include_directories(test_minion PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern
    ${CMAKE_SOURCE_DIR}/extern/entt
    ${CMAKE_SOURCE_DIR}/extern/vma
    ${CMAKE_SOURCE_DIR}/extern/tinygltf
)
target_link_libraries(test_minion PRIVATE glm::glm spdlog::spdlog Vulkan::Vulkan)
target_compile_definitions(test_minion PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    CONFIG_DIR="${CMAKE_SOURCE_DIR}/assets/config/"
    MAP_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/maps/"
)

add_executable(test_structure tests/test_structure.cpp
    src/structure/StructureSystem.cpp
    src/structure/StructureConfig.cpp
    src/map/MapLoader.cpp
)
target_include_directories(test_structure PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern
    ${CMAKE_SOURCE_DIR}/extern/entt
    ${CMAKE_SOURCE_DIR}/extern/vma
    ${CMAKE_SOURCE_DIR}/extern/tinygltf
)
target_link_libraries(test_structure PRIVATE glm::glm spdlog::spdlog Vulkan::Vulkan)
target_compile_definitions(test_structure PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    CONFIG_DIR="${CMAKE_SOURCE_DIR}/assets/config/"
    MAP_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/maps/"
)

add_executable(test_jungle tests/test_jungle.cpp
    src/jungle/JungleSystem.cpp
    src/jungle/JungleConfig.cpp
    src/map/MapLoader.cpp
)
target_include_directories(test_jungle PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern
    ${CMAKE_SOURCE_DIR}/extern/entt
    ${CMAKE_SOURCE_DIR}/extern/vma
    ${CMAKE_SOURCE_DIR}/extern/tinygltf
)
target_link_libraries(test_jungle PRIVATE glm::glm spdlog::spdlog Vulkan::Vulkan)
target_compile_definitions(test_jungle PRIVATE
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    CONFIG_DIR="${CMAKE_SOURCE_DIR}/assets/config/"
    MAP_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/maps/"
)

# ── AAA plan tests ────────────────────────────────────────────────────────────
set(_COMMON_INC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern
    ${CMAKE_SOURCE_DIR}/extern/entt
    ${CMAKE_SOURCE_DIR}/extern/vma
)
set(_COMMON_DEFS GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE)

add_executable(test_fixedpoint tests/test_fixedpoint.cpp
    src/math/FixedPoint.cpp
    src/math/DetRNG.cpp
)
target_include_directories(test_fixedpoint PRIVATE ${_COMMON_INC})
target_link_libraries(test_fixedpoint PRIVATE glm::glm)
target_compile_definitions(test_fixedpoint PRIVATE ${_COMMON_DEFS})

add_executable(test_simulation_loop tests/test_simulation_loop.cpp
    src/math/FixedPoint.cpp
    src/math/DetRNG.cpp
    src/math/MurmurHash3.cpp
    src/core/StateChecksumSystem.cpp
)
target_include_directories(test_simulation_loop PRIVATE ${_COMMON_INC})
target_link_libraries(test_simulation_loop PRIVATE glm::glm spdlog::spdlog)
target_compile_definitions(test_simulation_loop PRIVATE ${_COMMON_DEFS}
    $<$<BOOL:${GLORY_DETERMINISTIC}>:GLORY_DETERMINISTIC>)

add_executable(test_snapshot tests/test_snapshot.cpp
    src/math/FixedPoint.cpp
    src/math/DetRNG.cpp
    src/math/MurmurHash3.cpp
    src/core/StateChecksumSystem.cpp
    src/core/StateSnapshot.cpp
)
target_include_directories(test_snapshot PRIVATE ${_COMMON_INC})
target_link_libraries(test_snapshot PRIVATE glm::glm spdlog::spdlog)
target_compile_definitions(test_snapshot PRIVATE ${_COMMON_DEFS}
    $<$<BOOL:${GLORY_DETERMINISTIC}>:GLORY_DETERMINISTIC>)

add_executable(test_networking tests/test_networking.cpp
    src/net/Transport.cpp
    src/net/GameServer.cpp
    src/net/GameClient.cpp
    src/core/StateSnapshot.cpp
    src/core/StateChecksumSystem.cpp
    src/math/FixedPoint.cpp
    src/math/DetRNG.cpp
    src/math/MurmurHash3.cpp
)
target_include_directories(test_networking PRIVATE ${_COMMON_INC})
target_link_libraries(test_networking PRIVATE glm::glm spdlog::spdlog)
target_compile_definitions(test_networking PRIVATE ${_COMMON_DEFS})

add_executable(test_flowfield tests/test_flowfield.cpp
    src/nav/FlowField.cpp
    src/nav/HierarchicalFlowField.cpp
)
target_include_directories(test_flowfield PRIVATE ${_COMMON_INC})
target_link_libraries(test_flowfield PRIVATE glm::glm)
target_compile_definitions(test_flowfield PRIVATE ${_COMMON_DEFS})

add_executable(test_determinism tests/test_determinism.cpp
    src/math/FixedPoint.cpp
    src/math/DetRNG.cpp
    src/math/MurmurHash3.cpp
    src/core/StateChecksumSystem.cpp
)
target_include_directories(test_determinism PRIVATE ${_COMMON_INC})
target_link_libraries(test_determinism PRIVATE glm::glm spdlog::spdlog)
target_compile_definitions(test_determinism PRIVATE ${_COMMON_DEFS} GLORY_DETERMINISTIC)
