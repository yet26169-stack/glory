# Project: Vulkan Engine Base (Codename: Glory)

## 1. Tech Stack
- **Language:** C++20
- **Build System:** CMake (3.20+)
- **Graphics API:** Vulkan 1.3 (using VMA - Vulkan Memory Allocator)
- **Windowing:** GLFW
- **Math:** GLM + Custom Fixed-Point (`Fixed64`, `Fixed32`)
- **Logging:** spdlog
- **ECS:** EnTT
- **Networking:** ENet
- **Scripting:** Lua (via sol2)
- **Physics:** Internal deterministic engine
- **Audio:** miniaudio

## 2. Architecture Overview

The engine is split into the following logical modules:
- **Core:** Application entry point, main loop, simulation loop, threading, and system scheduler.
- **Windowing:** Wrapper for GLFW and Vulkan Surface.
- **Renderer:** Vulkan 1.3 backend, GPU-driven rendering, culling, and post-processing (Bloom, SSAO, SSR).
- **Ability:** Data-driven ability system (DoT, projectiles, status effects).
- **Animation:** Skeletal animation player with blending and retargeting.
- **Combat:** Combat state machine, economy, and structure management.
- **Nav:** Navigation via Recast/Detour (pathfinding) and Flow Fields.
- **Network:** Deterministic lockstep networking with rollback support.
- **VFX:** Visual effects system (GPU particles, trails, mesh effects).

## 3. Coding Standards
- **Naming:** `PascalCase` for Classes, `camelCase` for methods/variables, `m_` prefix for private members.
- **Error Handling:** Use `std::runtime_error` for Vulkan failures; informative `spdlog` messages in catch blocks.
- **Modern C++:** C++20 standard, heavy use of `std::unique_ptr` and `std::vector`.
- **Determinism:** Use fixed-point math (`Fixed64`) for all simulation logic to ensure cross-platform consistency.

## 4. Implementation Status
The following systems are fully implemented and verified with tests:
1. **Core Loop:** Main application and simulation ticks.
2. **Vulkan Backend:** Device selection, swapchain, and frames-in-flight sync.
3. **Advanced Rendering:** Shadow mapping, Hi-Z occlusion culling, Bloom, SSAO, and SSR.
4. **Navigation:** Pathfinding around obstacles using Recast/Detour.
5. **Ability & Combat:** Multi-stage ability execution and melee combat mechanics.
6. **Networking:** Low-latency input synchronization and state snapshots.

## Lifecycle Contract
Vulkan resources are destroyed in reverse-initialization order.
Class member declaration order mirrors this. The `Renderer` orchestrator manages
the lifetime of all GPU-dependent subsystems.

## Directory Structure

Glory/
├── CMakeLists.txt
├── extern/                         # EnTT, GLM, GLFW, spdlog, sol2, tinygltf, etc.
├── shaders/                        # GLSL source (compiled to SPIR-V at build time)
├── src/
│   ├── main.cpp                    # Application entry point
│   ├── ability/                    # Ability system and status effects
│   ├── animation/                  # Skeletal animation and blending
│   ├── assets/                     # Asset loading and cooked format support
│   ├── audio/                      # Spatial audio engine (miniaudio)
│   ├── camera/                     # Isometric camera controller
│   ├── combat/                     # Combat states, economy, and structures
│   ├── core/                       # Application loop, threading, scheduler, math
│   ├── fog/                        # Fog of War visibility and rendering
│   ├── hud/                        # Dear ImGui-based game HUD
│   ├── input/                      # Input mapping and targeting
│   ├── map/                        # Map data and symmetry logic
│   ├── math/                       # Deterministic fixed-point math
│   ├── nav/                        # Pathfinding, splines, and lane followers
│   ├── network/                    # Lockstep netcode and input sync
│   ├── physics/                    # Collision detection and integration
│   ├── renderer/                   # Vulkan 1.3 engine and render passes
│   ├── replay/                     # Deterministic replay system
│   ├── scene/                      # Scene graph and ECS components
│   ├── scripting/                  # Lua script engine and bindings
│   ├── terrain/                    # Terrain heightmaps and textures
│   ├── vfx/                        # Particle systems and trails
│   └── window/                     # GLFW window management
└── tests/                          # Unit test suite (combat, nav, animation, etc.)

## Shader Handling
- Shaders use GLSL `#version 450`.
- Compiled to SPIR-V via `glslc` (Vulkan SDK) during the CMake build.
- `*.spv` files are generated in the build directory and ignored by Git.
- `Pipeline` and specialized renderers load bytecode at runtime to create `VkShaderModule`.
