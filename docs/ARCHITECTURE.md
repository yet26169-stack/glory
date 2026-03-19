# Glory Engine — Architecture

> **Sources:** ENGINE_STRUCTURE_DEEP_DIVE, RENDERING_DOCUMENTATION_INDEX, RENDERING_SUMMARY, RENDERING_VFX_SYSTEM_DEEP_DIVE, GLORY_VULKAN_DEEP_ANALYSIS, CAMERA_PHYSICS, VFX_QUICK_START, docs_VFX_SYSTEM_IMPLEMENTATION_SPEC_Version2

---

## 1. Engine Structure

### 1.1 Overview

The **Glory Engine** is a high-performance custom C++ game engine built on Vulkan 1.3, designed specifically for MOBA (Multiplayer Online Battle Arena) and RTS games. It targets high entity counts (hundreds of units), low-latency simulation, and a modern GPU-driven rendering pipeline.

**Tech Stack:**
- **Language:** C++20
- **Build System:** CMake 3.20+
- **Graphics API:** Vulkan 1.3 (with VMA — Vulkan Memory Allocator)
- **Windowing:** GLFW
- **Math:** GLM + Custom Fixed-Point (`Fixed64`, `Fixed32`)
- **Logging:** spdlog
- **ECS:** EnTT
- **Networking:** ENet
- **Scripting:** Lua (via sol2)
- **Physics:** Internal deterministic engine
- **Audio:** miniaudio

### 1.2 Directory Structure

```
Glory/
├── CMakeLists.txt
├── CMakePresets.json
├── extern/                         # EnTT, GLM, GLFW, spdlog, sol2, tinygltf, etc.
├── shaders/                        # GLSL source (compiled to SPIR-V at build time)
├── assets/
│   ├── abilities/                  # JSON ability definitions
│   └── vfx/                        # JSON particle emitter definitions
├── src/
│   ├── main.cpp
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
└── tests/                          # Unit test suite
```

### 1.3 Module Architecture

The engine is split into the following logical modules:

| Module | Path | Responsibility |
|--------|------|---------------|
| **Core** | `src/core/` | Application entry, main loop, simulation loop, threading, system scheduler |
| **Windowing** | `src/window/` | GLFW and Vulkan Surface wrapper |
| **Renderer** | `src/renderer/` | Vulkan 1.3 backend, GPU-driven rendering, culling, post-processing |
| **Ability** | `src/ability/` | Data-driven ability system (DoT, projectiles, status effects) |
| **Animation** | `src/animation/` | Skeletal animation player with blending and retargeting |
| **Combat** | `src/combat/` | Combat state machine, economy, and structure management |
| **Nav** | `src/nav/` | Recast/Detour pathfinding, flow fields |
| **Network** | `src/network/` | Deterministic lockstep networking with rollback support |
| **VFX** | `src/vfx/` | Visual effects system (GPU particles, trails, mesh effects) |
| **Fog** | `src/fog/` | Fog of War visibility and rendering |
| **HUD** | `src/hud/` | Dear ImGui-based game HUD |

### 1.4 ECS (Entity Component System)

Powered by **EnTT**, the engine organizes data into components (`src/scene/Components.h`) and logic into systems:

- **Efficiency:** Data-oriented design minimizes cache misses via contiguous component storage
- **Modularity:** Systems like `MinionSystem`, `AbilitySystem`, and `ProjectileSystem` operate independently on the EnTT registry
- **Composition over inheritance:** Every gameplay entity is an `entt::entity` with composable components

**Core ECS Components (`src/scene/Components.h`):**

```cpp
// ── Transform ────────────────────────────────────────────────────────────────
struct TransformComponent {
    glm::vec3 position{0.0f};        // World-space XYZ
    glm::vec3 rotation{0.0f};        // Euler angles in radians (Y-X-Z order)
    glm::vec3 scale{1.0f};           // Per-axis scale

    glm::mat4 getModelMatrix() const {
        // Build model matrix: T * Ry * Rx * Rz * S
        glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
        m = glm::rotate(m, rotation.y, glm::vec3(0, 1, 0));
        m = glm::rotate(m, rotation.x, glm::vec3(1, 0, 0));
        m = glm::rotate(m, rotation.z, glm::vec3(0, 0, 1));
        m = glm::scale(m, scale);
        return m;
    }
};

// ── Mesh & Material ──────────────────────────────────────────────────────────
struct MeshComponent {
    uint32_t meshIndex = 0;          // Index into Scene::m_meshes
};

struct MaterialComponent {
    uint32_t materialIndex = 0;    // Diffuse texture index (in bindless array)
    uint32_t normalMapIndex = 0;   // Normal map texture index (0 = flat)
    float    shininess = 0.0f;     // Legacy Blinn-Phong shine
    float    metallic  = 0.0f;     // 0 = dielectric, 1 = metal
    float    roughness = 0.5f;     // 0.04–1.0
    float    emissive  = 0.0f;     // >0 = self-illumination
};

// ── GPU-Resident Skinned Mesh ───────────────────────────────────────────────
struct GPUSkinnedMeshComponent {
    uint32_t staticSkinnedMeshIndex = 0;  // Index into Scene::m_staticSkinnedMeshes
    uint32_t boneSlot = 0;               // Slot in ring-buffer bone SSBO (0..MAX_SKINNED_CHARS-1)
};

// ── Character Movement ────────────────────────────────────────────────────────
struct CharacterComponent {
    glm::vec3 targetPosition{0.0f};
    float     moveSpeed = 6.0f;
    bool      hasTarget = false;
    glm::quat currentFacing{1.0f, 0.0f, 0.0f, 0.0f};
    float     currentSpeed = 0.0f;
};
```

### 1.5 Main Loop & Simulation

The engine follows a **decoupled simulation/render loop**:

- **Application (`src/core/Application.cpp`):** Manages the window and high-level loop
- **Fixed-Timestep Simulation:** Runs at fixed **30 Hz** (`FIXED_DT = 1/30 s`) using an accumulator. Ensures deterministic behavior for gameplay (AI, projectiles, physics)
- **Vulkan Synchronization:** `Sync.h` manages "Frames in Flight" (`MAX_FRAMES_IN_FLIGHT = 2`)
- **`INITIAL_INSTANCE_CAPACITY = 1024`** — instance buffer pre-allocated size

**Simulation system execution order (per fixed tick):**

```
Tick N (FIXED_DT = 1/30 s):
  1.  InputIngestionSystem      — consume player commands queued since last tick
  2.  SceneUpdateSystem         — character movement, facing, animation state
  3.  AbilitySystem             — ability activations, cooldowns, resources
  4.  ProjectileSystem          — advance positions, check collision
  5.  MinionSystem              — AI state, lane following, aggro, targeting
  6.  StructureSystem           — tower attack cycle, inhibitor/nexus health
  7.  JungleSystem              — camp spawning, monster AI, respawn timers
  8.  AutoAttackSystem          — target acquisition, damage application
  9.  StatusEffectSystem        — DoT/HoT ticks, buff/debuff duration
  10. CooldownSystem            — decrement cooldown timers
  11. EffectSystem              — flush pending effects
  12. MovementSystem            — apply SimVelocity to SimPosition
  13. CollisionResolutionSystem — resolve entity overlaps
  14. DeathProcessingSystem     — remove dead entities, trigger rewards
  15. StateChecksumSystem       — MurmurHash3 over all Sim* components
```

### 1.6 Renderer File Architecture

```
src/renderer/ (~12,685 lines)

CORE VULKAN INFRASTRUCTURE:
├─ Renderer.h/cpp (1,300+ lines)         ← Main frame loop, command recording
├─ Context.h/cpp (200 lines)             ← VkInstance creation, debug setup
├─ Device.h/cpp (350+ lines)             ← VkDevice, VkPhysicalDevice, queues
├─ Swapchain.h/cpp (150+ lines)          ← VkSwapchainKHR, image views
├─ Pipeline.h/cpp (200+ lines)           ← Graphics pipeline, renderpass
├─ Descriptors.h/cpp (250+ lines)        ← Descriptor sets, UBOs, bindless
├─ Texture.h/cpp (400+ lines)            ← VkImage, VkSampler, STB loading
├─ Buffer.h/cpp (150+ lines)             ← VMA-backed GPU buffers
├─ Sync.h/cpp (100+ lines)               ← Fences, semaphores, frame sync
├─ HDRFramebuffer.h/cpp (200+ lines)     ← HDR targets + bloom extraction
├─ BloomPass.h/cpp (150+ lines)          ← Bloom compute + blur
└─ ToneMapPass.h/cpp (100+ lines)        ← HDR→SDR tone-mapping

SPECIAL RENDERERS (VFX/Abilities):
├─ ClickIndicatorRenderer.h/cpp          ← Click feedback UI animation
├─ GroundDecalRenderer.h/cpp             ← Decal system with lifetime
├─ DistortionRenderer.h/cpp              ← Post-process distortion
├─ ShieldBubbleRenderer.h/cpp            ← Transparent shield effect
├─ ConeAbilityRenderer.h/cpp             ← W-ability cone mesh
├─ ExplosionRenderer.h/cpp               ← E-ability explosions
├─ SpriteEffectRenderer.h/cpp            ← Sprite atlas VFX
├─ DynamicMesh.h/cpp                     ← Runtime mesh generation
├─ StaticSkinnedMesh.h/cpp               ← GPU-skinned character meshes
├─ Model.h/cpp                           ← Mesh loading (OBJ, GLB)
└─ DebugRenderer.h/cpp (src/nav/)        ← Debug shapes/lines
```

### 1.7 Shader Files

All shader source files are in `/shaders/` and compiled to `.spv` (SPIR-V) bytecode via `glslc` during the CMake build.

```
shaders/
├── bloom_blur.frag / bloom_extract.frag
├── debug.frag / debug.vert
├── deferred.frag / deferred.vert
├── fog.frag / fog.vert               ← Fog of War shaders
├── gbuffer.frag / gbuffer.vert
├── grid.frag / grid.vert
├── particle.frag / particle.vert     ← Billboard particle rendering
├── particle_sim.comp                 ← GPU particle simulation
├── postprocess.frag / postprocess.vert
├── shadow.frag / shadow.vert
├── sky.frag
├── ssao.frag / ssao_blur.frag
├── terrain.frag / terrain.vert
├── triangle.frag / triangle.vert     ← Main PBR model shader
├── skinned.vert                      ← GPU skeletal skinning
└── water.frag / water.vert
```

### 1.8 External Dependencies

| Library | Path | Status |
|---------|------|--------|
| EnTT | `extern/entt/` | **INTEGRATED** |
| ImGui | `extern/imgui/` | **INTEGRATED** |
| nlohmann/json | `extern/nlohmann/` | **INTEGRATED** |
| stb | `extern/stb/` | **INTEGRATED** |
| TinyGLTF | `extern/tinygltf/` | **INTEGRATED** |
| TinyOBJ | `extern/tinyobj/` | **INTEGRATED** |
| VMA (Vulkan Memory Allocator) | `extern/vma/` | **INTEGRATED** |
| meshoptimizer | `extern/meshopt/` | **INTEGRATED** |

### 1.9 Existing Tests

```
tests/
├── test_ability.cpp
├── test_fog.cpp
├── test_maploader.cpp
├── test_mirror.cpp
├── test_nav.cpp
└── test_terrain.cpp
```

**Build & test gate:**
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## 2. Vulkan Rendering Pipeline

### 2.1 Vulkan Initialization

#### VkInstance Creation (`Context.cpp:82-120`)
- Target: **Vulkan 1.3 API**
- Extensions: Platform-specific (`KHR_surface`) + `VK_EXT_debug_utils`
- Validation: `VK_LAYER_KHRONOS_validation` (debug builds only)
- macOS: `VK_KHR_portability_enumeration` + `portability_subset`

#### Physical Device Selection (`Device.cpp:69-93`)
- Scoring: Discrete GPU +10,000 → Integrated +1,000 → max dimension bonus
- Minimum: Vulkan 1.3 support required
- Extensions: `VK_KHR_swapchain` + portability (macOS)
- Preferred: Dedicated DMA queue (NVIDIA family 2, AMD family 1)

#### Logical Device Creation (`Device.cpp:174-250`)

Queue Families:
- **Graphics:** render + compute
- **Present:** swapchain presentation
- **Transfer:** dedicated DMA (fallback: graphics family)

Physical Features Enabled:
- `samplerAnisotropy: true`
- `fillModeNonSolid: true` (wireframe mode)
- `multiDrawIndirect: true`

Vulkan 1.2 Features (CRITICAL):
- `drawIndirectCount`: OPTIONAL (not on MoltenVK/Apple Silicon)
- `descriptorBindingSampledImageUpdateAfterBind: true`
- `shaderSampledImageArrayNonUniformIndexing: true`
- `descriptorBindingPartiallyBound: true`
- `runtimeDescriptorArray: true`

#### Memory Allocator
- **VMA (Vulkan Memory Allocator)** for all GPU buffers
- Default strategies: host-visible for UBOs/SSBOs, device-local for meshes
- Persistent mapping for frequent uploads

#### Swapchain
- Format: SRGB preferred (`B8G8R8A8_SRGB` or `R8G8B8A8_SRGB`)
- Present Mode: `VK_PRESENT_MODE_FIFO_KHR` (triple-buffering)
- Supports recreation on window resize

### 2.2 Frame Synchronization

```
Frame Sync (Renderer.cpp:255-350):
  1. vkWaitForFences(inFlightFence)        ← GPU⇒CPU: wait oldest frame
  2. vkAcquireNextImageKHR(swapchain)      ← Get swapchain image index
  3. recordCommandBuffer(cmd, imageIndex, dt)
  4. vkResetFences(inFlightFence)
  5. vkQueueSubmit(cmd, imgSem→renderSem, inFlightFence)
  6. vkPresentKHR(swapchain)
```

Multi-Frame-in-Flight: `MAX_FRAMES_IN_FLIGHT = 2`

### 2.3 Command Buffer — Full Frame Pipeline

The entire pipeline executes single-threaded in `drawFrame()` (main thread), with plans for multi-threaded recording.

**Render Timeline (60 FPS target):**

```
T=0ms:  glfwPollEvents()
T=1ms:  drawFrame() called
        ├─ Update game systems (abilities, physics, AI)
        ├─ vfxRenderer.processQueue()    [SPSC event drain]
        ├─ Update particle emitters
        └─ dt capped at 50ms (20 FPS floor)

T=2ms:  GPU Synchronization
        ├─ vkWaitForFences(frame[n-2])
        ├─ vkAcquireNextImageKHR()
        └─ Ready to record command buffer

T=3ms:  Record Command Buffer
        ├─ Update per-frame UBOs
        ├─ Particle compute dispatch
        ├─ Opaque render pass
        ├─ Transparent render pass
        ├─ Bloom extraction & blur
        ├─ Tone-map to swapchain
        └─ ImGui overlay

T=4ms:  Queue Submission & GPU Execution
        ├─ vkResetFences(frame[n])
        ├─ vkQueueSubmit(...)
        └─ CPU proceeds to next frame

T=5ms:  Present to Screen
        └─ vkPresentKHR(renderFinishedSemaphore)

T=16ms: Next Frame (60 FPS)
```

### 2.4 Main Command Buffer Structure

```
┌──────────────────────────────────────────────────────────────────────┐
│ PHASE 0: CPU Updates (Outside GPU recording)                         │
├──────────────────────────────────────────────────────────────────────┤
│ ├─ vfxRenderer.processQueue()          [SPSC: game⇒render events]   │
│ ├─ vfxRenderer.update(dt)              [CPU: emission state]         │
│ ├─ abilitySystem.update()              [Ability state machines]      │
│ ├─ projectileSystem.update()           [Physics integration]         │
│ ├─ combatSystem.update()               [Auto-attack timers]          │
│ └─ groundDecalRenderer.update()        [Decal lifetime]              │
└──────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│ PHASE 1: GPU Compute Shaders (OUTSIDE render pass)                   │
├──────────────────────────────────────────────────────────────────────┤
│ ├─ vfxRenderer.dispatchCompute(cmd)    [particle_sim.comp]           │
│ │  └─ Simulates gravity, wind, lifetime for ALL particles            │
│ └─ vfxRenderer.barrierComputeToGraphics(cmd)  [SSBO write→read]     │
└──────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│ PHASE 2: Main Opaque Render Pass (HDR Framebuffer)                   │
├──────────────────────────────────────────────────────────────────────┤
│ vkCmdBeginRenderPass(hdrFB, CLEAR)                                   │
│                                                                       │
│ Per-Frame UBO Updates:                                               │
│ ├─ view = isoCam.getViewMatrix()                                     │
│ ├─ proj = isoCam.getProjectionMatrix(aspect)                         │
│ ├─ lightSpaceMatrix (shadows)                                        │
│ ├─ lights[0].position = (100, 60, 100), color = (1.0, 0.95, 0.85)  │
│ └─ fog: density, color, start, end                                   │
│                                                                       │
│ Static Mesh Pass:                                                    │
│ ├─ vkCmdBindPipeline(mainPipeline or wireframePipeline)             │
│ ├─ For each MeshComponent (no GPUSkinnedMeshComponent):             │
│ │  ├─ instance.model = transform.getModelMatrix()                   │
│ │  └─ mesh.draw(cmd)  ← vkCmdDrawIndexed                           │
│                                                                       │
│ GPU-Skinned Mesh Pass:                                               │
│ ├─ vkCmdBindPipeline(skinnedPipeline)                               │
│ ├─ For each GPUSkinnedMeshComponent:                                │
│ │  ├─ vkCmdPushConstants(boneBaseIndex)                              │
│ │  └─ mesh.draw(cmd)  ← skinned.vert                               │
│                                                                       │
│ Ground Decals, Click Indicator, Debug Shapes, Grid                   │
│ vkCmdEndRenderPass(hdrFB)                                            │
└──────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│ PHASE 3: Transparent/VFX Render Pass (LOAD_OP_LOAD)                 │
├──────────────────────────────────────────────────────────────────────┤
│ m_hdrFB->copyColor(cmd)  [Pre-distortion color copy]                 │
│ vkCmdBeginRenderPass(hdrFB, LOAD_OP_LOAD)                           │
│                                                                       │
│ VFX Particle Billboards, Trail Ribbons, Shield Bubble,              │
│ W-Ability Cone, E-Ability Explosions, Mesh Effects, Sprite VFX,     │
│ Distortion Pass                                                       │
│                                                                       │
│ vkCmdEndRenderPass(hdrFB)                                            │
└──────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│ PHASE 4: Bloom Extraction & Blur (OUTSIDE render pass)              │
├──────────────────────────────────────────────────────────────────────┤
│ m_bloom->dispatch(cmd)  [bloom_extract.comp → bloom_blur.comp]      │
└──────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│ PHASE 5: Tone-Map to Swapchain (Final composite)                    │
├──────────────────────────────────────────────────────────────────────┤
│ vkCmdBeginRenderPass(swapchainRenderPass)                            │
│ ├─ m_toneMap->render(cmd, exposure=1.0f, bloomStrength=0.3f)       │
│ └─ ImGui_ImplVulkan_RenderDrawData(cmd)                             │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.5 Descriptor Set Layout

```
binding 0: UBO (view, proj, lightSpaceMatrix)
binding 1: Bindless texture array (diffuse + normal maps, up to 64)
binding 2: Light UBO (point lights, fog params, ambient)
binding 3: Shadow map (1× combined sampler)
binding 4: Bone matrix SSBO (GPU skinning — MAX_CHARS=128 × MAX_BONES=256)
```

Bindless Texture Array:
- Array size: `MAX_BINDLESS_TEXTURES = 64` (extended to 4096 in newer bindless system)
- `set=1, binding=0` — `layout(...) uniform sampler2D textures[4096]`
- Requires `GL_EXT_nonuniform_qualifier` + `nonuniformEXT()` indexing

```cpp
struct InstanceData {
    glm::mat4 model;
    glm::mat4 normalMatrix;
    glm::vec4 tint;
    glm::vec4 params;        // shininess, metallic, roughness, emissive
    glm::vec4 texIndices;    // [0]=diffuse_idx, [1]=normal_idx
};
```

GpuObjectData SSBO (binding 7, set 0):
```cpp
struct GpuObjectData {
    glm::mat4 model;           // 64 bytes
    glm::mat4 normalMatrix;    // 64 bytes
    glm::vec4 aabbMin;         // 16 bytes
    glm::vec4 aabbMax;         // 16 bytes
    glm::vec4 tint;            // 16 bytes
    glm::vec4 params;          // 16 bytes {shininess, metallic, roughness, emissive}
    glm::vec4 texIndices;      // 16 bytes {diffuseIdx, normalIdx, 0, 0}
    uint32_t  meshVertexOffset;// 4 bytes
    uint32_t  meshIndexOffset; // 4 bytes
    uint32_t  meshIndexCount;  // 4 bytes
    uint32_t  _pad;            // 4 bytes
};  // Total: 224 bytes
```

### 2.6 Model Rendering Shaders

#### `triangle.vert` (Standard Model Vertex Shader)
- **Purpose:** Forward pass skinned/static model rendering
- **Attributes:** Position, Color, Normal, TexCoord (per-vertex); Model, Normal matrix (per-instance); Material params; Texture indices (bindless lookup)
- **Output:** World position, normal, light-space position

#### `triangle.frag` (PBR Fragment Shader)
Full Cook-Torrance BRDF with:
- **Cook-Torrance BRDF** (GGX normal distribution, Schlick-GGX geometry)
- **Fresnel-Schlick** approximation
- **Bindless texture array** (up to 64–4096 textures)
- **Shadow mapping** (16-sample Poisson disk soft shadows)
- **Normal mapping** via cotangent-frame TBN
- **Subsurface scattering** (wrap lighting for skin)
- **Fresnel rim lighting**
- **Thin-film iridescence** (metallic surfaces)
- **Clearcoat layer** (car paint effect)
- **Emissive glow**
- **Micro AO** (curvature-based)
- **Distance fog** (exponential with color blending)

Fragment shader texture lookup:
```glsl
// Bindless array
layout(set = 1, binding = 0) uniform sampler2D textures[4096];
#extension GL_EXT_nonuniform_qualifier : require

// Lookup
vec4 diffuse = texture(textures[nonuniformEXT(fragDiffuseIdx)], fragTexCoord);
vec3 mapN = texture(textures[nonuniformEXT(fragNormalIdx)], fragTexCoord).rgb * 2.0 - 1.0;
```

#### `skinned.vert` (GPU Skinning Vertex Shader)
- Up to 4 bone influences per vertex (weights sum to 1.0)
- Joint indices local to entity (offset by `boneBaseIndex` push constant)
- Large shared bone SSBO (`MAX_CHARS × MAX_BONES × mat4`)

```glsl
uint base = boneBaseIndex;
skinMat = bones[base + joints.x] * weights.x
        + bones[base + joints.y] * weights.y
        + bones[base + joints.z] * weights.z
        + bones[base + joints.w] * weights.w;

skinnedPos = skinMat * position;   // bind-pose → skinned local
worldPos   = model   * skinnedPos; // → world space
```

### 2.7 GPU-Driven Rendering (GpuCuller)

Implemented in `src/renderer/GpuCuller.h/.cpp`:

- **Compute Culling:** `cull.comp` tests every object's AABB against the camera frustum on the GPU
- **Indirect Drawing:** `vkCmdDrawIndexedIndirectCount` — GPU writes its own draw commands into an indirect buffer
- Extended with instance output SSBO: compute shader writes per-surviving-instance data (model matrix + tint) in parallel with draw commands
- All four outputs (cull + indirect draw + instance data + count) produced in one compute dispatch

### 2.8 Cascaded Shadow Maps

Implemented in `CascadeShadow.cpp`:
- 3 cascades (10m / 40m / 120m splits)
- 2048×2048 depth array image
- Per-cascade frustum fit with bounding-sphere + texel-snap to prevent edge shimmer
- Front-face culling + depth bias in pipeline
- Subpass dependencies handle layout transitions

Shadow status: **PLACEHOLDER ONLY — NOT FULLY IMPLEMENTED** for real cascaded path:
```cpp
// Dummy shadow in Renderer.cpp:
m_dummyShadow = Texture::createDefault(*m_device);
m_descriptors->updateShadowMap(m_dummyShadow.getImageView(), m_dummyShadow.getSampler());
ubo.lightSpaceMatrix = glm::mat4(1.0f);  // identity = no shadow
```

### 2.9 Texture & Resource Management

**Procedural Texture Factories (Texture.cpp:200+):**
```cpp
Texture::createDefault()          // 1×1 white
Texture::createCheckerboard()     // Checkered pattern
Texture::createFlatNormal()       // (0, 0, 1) neutral normal
Texture::createBrickNormal()      // Procedural brick bumps
Texture::createMarble()           // Procedural marble (SRGB)
Texture::createWood()             // Procedural wood grain
Texture::createLava()             // Procedural lava flow
Texture::createRock()             // Procedural rock texture
Texture::createBrushedMetal()     // Brushed metal effect
Texture::createTiles()            // Procedural tiles
Texture::createCircuit()          // Circuit board texture
Texture::createHexGrid()          // Hexagonal grid
Texture::createGradient()         // Color gradient
Texture::createNoise()            // Perlin noise
```

**Async Texture Streaming (`TextureStreamer.h/.cpp`):**
- Background worker thread decodes images with stb_image
- Allocates staging buffer, uploads via dedicated transfer queue
- Generates full mip chain with `vkCmdBlitImage`
- Waits on `VkFence` polled each frame in `tick()`
- Ready callback fires on main thread to update bindless descriptor slot
- Zero graphics-queue stalls
- Correctly uses `VK_SHARING_MODE_CONCURRENT` for multi-queue-family images

### 2.10 Performance Limits

| Component | Limit | Notes |
|-----------|-------|-------|
| Max particles/emitter | 2,048 | SSBO size (64 bytes × 2048) |
| Max concurrent emitters | 32 | Descriptor set pre-allocation |
| Max instances/draw | 256 | Per-frame instance buffer |
| Max bindless textures | 64–4096 | Descriptor array bound |
| Max bones/character | 256 | SSBO ring-buffer entry |
| Max skinned characters | 128 | Ring-buffer slots |
| Max point lights | 4 | Hard-coded in LightUBO |
| Max ground decals | 512 | GroundDecalRenderer internal |
| Compute thread group | 64 | Local work group size (warp-aligned) |

Typical frame breakdown (1920×1080):
```
CPU update:   2-4ms  (game logic, animation, physics)
GPU compute:  1-2ms  (particle simulation)
GPU graphics: 8-12ms (rendering 2-3 render passes)
Total:        12-16ms (60 FPS target)
```

### 2.11 Key Architectural Decisions & Tradeoffs

| Decision | Rationale | Tradeoff |
|----------|-----------|----------|
| Single-threaded command recording | Simplicity, no synchronization overhead | No parallelized draw call recording |
| VFX SPSC queue (lock-free) | Zero-copy, minimal latency | Game⇒Render is asynchronous (1-frame delay) |
| Persistent UBO/SSBO mapping | Avoid CPU→GPU round-trip | Must use explicit flush() |
| Bindless textures | Material reuse, reduced descriptor updates | Fixed array size limit |
| Dedicated transfer queue | DMA offload on discrete GPUs | Falls back to graphics queue |
| Compute-outside-renderpass | Avoid render pass incompatibility | Extra command buffer complexity |
| Two render passes (HDR + LOAD) | Layered rendering (opaque then transparent) | RenderPass object complexity |
| Fixed MAX_INSTANCES=256 | Simple CPU→GPU data transfer | Limits entities per frame |

---

## 3. Post-Process & Render Passes

### 3.1 HDR Framebuffer

`HDRFramebuffer.h/.cpp` manages HDR render targets:
- HDR color target: `R16G16B16A16_SFLOAT`
- Depth target: `D32_SFLOAT`
- `copyColor(cmd)` — pre-distortion color copy for refractive effects

### 3.2 Bloom

`BloomPass.h/.cpp`:
- 3-pass pipeline: bright pixel extraction (soft knee threshold) → horizontal 9-tap Gaussian blur → vertical Gaussian blur at half resolution
- Composited with HDR scene in tone-mapping pass
- ImGui sliders for bloom intensity (0–2) and threshold (0.1–5)
- **In MOBA mode:** skipped (`m_bloom`/`m_ssao` unique_ptrs never created)
- Bound with 1×1 dummy images to descriptors when skipped

### 3.3 SSAO (Screen-Space Ambient Occlusion)

`SSAO.h/.cpp`:
- 16 hemisphere samples per pixel at half resolution + blur pass
- **In MOBA mode:** disabled to maximize performance
- Reads depth + normals

### 3.4 Post-Process Pipeline

`PostProcess.h/.cpp` (HDR + FXAA + ACES tone map + specialization variants):

Two pipeline variants compiled at startup:
- **`m_pipeline`:** All 13 effects enabled (bloom, SSAO, chroma, god rays, DoF, auto-exp, heat, outline, sharpen, grain, dither, FXAA, tone map)
- **`m_mobaPipeline`:** Only FXAA + ACES tone map — all other branches eliminated by driver at pipeline creation

### 3.5 Distance Fog

Fog parameters (`Descriptors.h`, `LightUBO`):
```cpp
struct LightUBO {
    // ... lights ...
    alignas(16) glm::vec3 fogColor{0.6f, 0.65f, 0.75f};  // Blue-gray
    alignas(4)  float     fogDensity       = 0.03f;
    alignas(4)  float     fogStart         = 5.0f;        // meters
    alignas(4)  float     fogEnd           = 50.0f;       // meters
};
```

Fragment shader implementation (`triangle.frag`, lines 281–285):
```glsl
float fogDist = length(viewPos - worldPos);
float fogFactor = exp(-fogDensity * max(fogDist - fogStart, 0.0));
fogFactor = clamp(fogFactor, 0.0, 1.0);
result = mix(fogColor, result, fogFactor);
```

Current defaults:
- **Color:** Blueish-gray (0.6, 0.65, 0.75)
- **Density:** 0.03 (moderate fog)
- **Start:** 5m  **End:** 50m

To adjust fog in C++:
```cpp
LightUBO lightData{};
lightData.fogColor   = glm::vec3(0.6f, 0.65f, 0.75f);
lightData.fogDensity = 0.03f;
lightData.fogStart   = 5.0f;
lightData.fogEnd     = 50.0f;
m_descriptors->updateLightBuffer(frameIndex, lightData);
```

### 3.6 Debug & Overlay Renderers

#### DebugRenderer (`src/nav/DebugRenderer.h/.cpp`)

Immediate-mode debug visualization:
```cpp
class DebugRenderer {
public:
    void init(const Device& device, VkRenderPass renderPass);

    void drawLine(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color);
    void drawCircle(const glm::vec3& center, float radius, const glm::vec4& color, int segments=32);
    void drawAABB(const glm::vec3& min, const glm::vec3& max, const glm::vec4& color);
    void drawSphere(const glm::vec3& center, float radius, const glm::vec4& color, int segments=8);

    void render(VkCommandBuffer cmd, const glm::mat4& viewProj);
    void clear();
};
```

Implementation: CPU-side vertex buffer (dynamic, CPU_TO_GPU), single VkPipeline (`debug.vert/frag`), line topology, auto-grow buffer if capacity exceeded.

#### Grid Shader (`grid.vert/frag`)
- Fullscreen world-space grid at Y=gridY
- Fine grid (1 unit) + coarse grid (5 units)
- Distance fade (15–40 unit range)
- Axis highlighting (red X, blue Z)
- Anti-aliased grid lines via `fwidth()`

---

## 4. VFX System

### 4.1 Architecture Overview

```
InputSystem ──────────────────────────────────────────►
                                                      AbilitySystem
                                                        │ (READY→CASTING→EXECUTING→ON_COOLDOWN)
                                                        │
                                                        ▼
                                                VFXEventQueue  (SPSC ring buffer, 256 slots)
                                                        │
                                                        ▼
VFXRenderer::processQueue()  ──────►  ParticleSystem (per effect, GPU SSBO)
VFXRenderer::update()        ──────►  CPU emission (spawn GpuParticle entries)
VFXRenderer::dispatchCompute()──────► compute shader (particle_sim.comp)
                                                barrier (compute → vertex)
VFXRenderer::render()        ──────►  particle.vert / particle.frag
```

Per-frame update order:

| Step | Call | Stage |
|------|------|-------|
| 1 | `AbilitySystem::update(reg, dt)` | Game logic |
| 2 | `VFXRenderer::processQueue(queue)` | Render thread |
| 3 | `VFXRenderer::update(dt)` | CPU emission |
| 4 | `VFXRenderer::dispatchCompute(cmd)` | **Outside** render pass |
| 5 | `VFXRenderer::barrierComputeToGraphics(cmd)` | Pipeline barrier |
| 6 | `VFXRenderer::render(cmd, viewProj, camRight, camUp)` | **Inside** render pass |

### 4.2 Core Types & Data Structures

**Location:** `src/vfx/VFXTypes.h`

#### GpuParticle (64 bytes, SSBO-aligned)
```cpp
struct alignas(16) GpuParticle {
    glm::vec4 posLife;   // xyz = world pos,  w = remaining lifetime (s)
    glm::vec4 velAge;    // xyz = velocity (m/s), w = age (s)
    glm::vec4 color;     // rgba (alpha fades to 0 at death)
    glm::vec4 params;    // x=size, y=rotation, z=atlasFrame, w=active(1)/dead(0)
};
static_assert(sizeof(GpuParticle) == 64);
```

GLSL mirror:
```glsl
struct Particle {
    vec4 posLife;   // xyz = world pos,  w = remaining lifetime (s)
    vec4 velAge;    // xyz = velocity,   w = age (s)
    vec4 color;     // rgba
    vec4 params;    // x=size, y=rotation, z=atlasFrame, w=active(1)/dead(0)
};
```

#### EmitterDef (Data-Driven Configuration)
```cpp
struct EmitterDef {
    std::string  id;                           // "vfx_fireball_cast"
    std::string  textureAtlas;                 // "textures/particles/fire.png"
    uint32_t     maxParticles    = 256;
    float        emitRate        = 40.0f;      // particles/second
    float        burstCount      = 0.0f;       // instant spawn on first frame
    bool         looping         = false;
    float        duration        = 1.5f;
    float        lifetimeMin     = 0.8f;
    float        lifetimeMax     = 1.6f;
    float        initialSpeedMin = 2.0f;
    float        initialSpeedMax = 6.0f;
    float        sizeMin         = 0.2f;
    float        sizeMax         = 0.6f;
    float        spreadAngle     = 45.0f;      // cone half-angle in degrees
    float        gravity         = 4.0f;       // m/s² downward
    std::vector<ColorKey> colorOverLifetime;
    std::vector<FloatKey> sizeOverLifetime;
};
```

#### VFXEvent (Game→Render Thread Communication)
```cpp
enum class VFXEventType : uint8_t {
    Spawn,    // Create new effect
    Destroy,  // Force-stop early
    Move,     // Teleport to new position
};

struct VFXEvent {
    VFXEventType type        = VFXEventType::Spawn;
    uint32_t     handle      = 0;             // output on Spawn, input on others
    char         effectID[48]{};              // e.g., "vfx_fireball_cast"
    glm::vec3    position    {0.f};
    glm::vec3    direction   {0.f, 1.f, 0.f};
    float        scale       = 1.0f;
    float        lifetime    = -1.0f;         // <0 = use EmitterDef.duration
};
```

### 4.3 SPSC Event Queue

**Location:** `src/vfx/VFXEventQueue.h`

- **Type:** Lock-free Single-Producer Single-Consumer ring buffer
- **Capacity:** 256 slots (power-of-two)
- **Memory Ordering:**
  - Producer: `push()` uses `memory_order_release` on head
  - Consumer: `pop()` uses `memory_order_release` on tail
  - Cache-line aligned (`alignas(64)`) to prevent false sharing

```cpp
template<uint32_t Capacity>
class SPSCQueue {
    bool push(const VFXEvent& ev) noexcept;  // Game thread
    bool pop(VFXEvent& out) noexcept;        // Render thread
    bool empty() const noexcept;
};
using VFXEventQueue = SPSCQueue<256>;
```

Benefits:
- Zero mutex overhead (atomics only)
- Cache-line padding prevents false sharing
- No allocation (fixed 256-slot ring buffer)
- Bounded latency (no malloc/free in hot path)

### 4.4 ParticleSystem (Single Active Effect)

**Location:** `src/vfx/ParticleSystem.h/.cpp`

```cpp
class ParticleSystem {
    // GPU SSBO (persistent CPU mapping via VMA_MEMORY_USAGE_CPU_TO_GPU)
    Buffer m_ssboBuffer;
    GpuParticle* m_particles;  // mapped pointer for CPU writes

    glm::vec3 m_position, m_direction;
    float m_scale, m_timeAlive, m_duration, m_emitAccum;
    bool  m_looping, m_stopped;

    void update(float dt);
    void spawnParticle();  // Find dead slot, randomize spawn params

    VkDescriptorSet m_descSet;  // Binding 0=SSBO, 1=atlas
};
```

**Particle Spawning (CPU, CPU_TO_GPU memory):**
1. Find first dead slot (`params.w < 0.5`)
2. Randomize velocity inside cone around direction (using spherical coords)
3. Randomize lifetime, size, initial color
4. Write to SSBO at mapped pointer (no GPU sync needed)
5. Repeat up to `emitRate * dt` times per frame

Memory strategy:
- **Apple Silicon (MoltenVK, unified memory):** zero-copy overhead
- **Discrete GPUs:** lives in BAR/ReBAR memory for direct CPU+GPU access

### 4.5 VFXRenderer (Master Orchestrator)

**Location:** `src/vfx/VFXRenderer.h/.cpp`

```cpp
class VFXRenderer {
    VkDescriptorPool      m_descPool;
    VkDescriptorSetLayout m_descLayout;
    VkPipeline m_computePipeline;   // particle_sim.comp
    VkPipeline m_renderPipeline;    // particle.vert/frag

    void processQueue(VFXEventQueue& queue);           // Drain events
    void update(float dt);                             // Emit particles
    void dispatchCompute(VkCommandBuffer cmd);         // GPU simulation
    void barrierComputeToGraphics(VkCommandBuffer cmd); // Synchronize
    void render(VkCommandBuffer cmd, ...);             // Draw billboards

    void registerEmitter(EmitterDef def);
    void loadEmitterDirectory(const std::string& path);  // JSON hot-load

private:
    std::vector<ParticleSystem> m_effects;                       // Active emitters (max 32)
    std::unordered_map<std::string, Texture*> m_atlasCache;
};
```

### 4.6 Descriptor Set Layout (VFX)

Both compute and graphics pipelines share the **same** layout:

| Binding | Type | Stages | Purpose |
|---------|------|--------|---------|
| 0 | `STORAGE_BUFFER` | Compute (RW) + Vertex (R) | Particle SSBO |
| 1 | `COMBINED_IMAGE_SAMPLER` | Fragment (R) | Particle atlas texture |

One descriptor set per active emitter, pre-allocated from shared pool (`MAX_CONCURRENT_EMITTERS=32`).

### 4.7 GPU Particle Simulation — `particle_sim.comp`

- **Local size:** 64 threads (warp/wavefront-aligned for AMD & NVIDIA)
- **Algorithm:**
  1. Skip dead particles (early exit: `params.w < 0.5`)
  2. Decrement `posLife.w` (lifetime), increment `velAge.w` (age)
  3. If lifetime ≤ 0 → mark dead (`params.w = 0`)
  4. Apply gravity: `velAge.y -= gravity * dt`
  5. Euler integrate: `posLife.xyz += velAge.xyz * dt`
  6. Fade alpha linearly: `color.a = lifeFrac`

Push Constants:
```cpp
struct SimPC {
    float    dt;
    float    gravity;
    uint32_t count;
    float    _pad;
};
```

Extensions for wind / drag / rotation:
```glsl
// Wind force
vec3 wind = vec3(sin(p.velAge.w) * 0.5, 0.0, 0.0);
p.velAge.xyz += wind * dt;

// Drag / air resistance
p.velAge.xyz *= 0.98;  // 2% damping per frame

// Rotation
p.params.y += angularVel * dt;
```

### 4.8 Billboard Vertex Shader — `particle.vert`

No vertex buffer is bound. The vertex shader reads the SSBO directly:

```glsl
uint pi = gl_VertexIndex / 6u;   // particle index
uint vi = gl_VertexIndex % 6u;   // vertex within billboard quad

// Expand in camera space
worldPos = particle.pos + camRight * offset.x * size + camUp * offset.y * size;
```

Dead particles output `gl_Position.z = 2.0` (clipped silently).

Push constant layout (96 bytes, within Vulkan's 128-byte guarantee):

| Field | Size |
|-------|------|
| `viewProj` (mat4) | 64 bytes |
| `camRight` (vec4) | 16 bytes |
| `camUp` (vec4) | 16 bytes |

### 4.9 VFX Quick Start Guide

**Step 1: Create EmitterDef JSON** (`assets/vfx/vfx_my_effect.json`):
```json
{
  "id": "vfx_my_effect",
  "textureAtlas": "textures/particles/my_atlas.png",
  "maxParticles": 128,
  "emitRate": 50.0,
  "burstCount": 10.0,
  "looping": false,
  "duration": 1.0,
  "lifetimeMin": 0.5,
  "lifetimeMax": 1.5,
  "initialSpeedMin": 2.0,
  "initialSpeedMax": 8.0,
  "sizeMin": 0.25,
  "sizeMax": 0.75,
  "spreadAngle": 30.0,
  "gravity": 3.0,
  "colorOverLifetime": [
    {"time": 0.0, "color": [1.0, 1.0, 1.0, 1.0]},
    {"time": 1.0, "color": [1.0, 1.0, 1.0, 0.0]}
  ]
}
```

**Step 2: Load in game code:**
```cpp
m_vfxRenderer->loadEmitterDirectory(std::string(ASSET_DIR) + "vfx/");
// Or register manually:
EmitterDef def{...};
m_vfxRenderer->registerEmitter(def);
```

**Step 3: Emit from ability system** (in ability JSON):
```json
{
  "castVFX":      "vfx_my_effect",
  "projectileVFX":"vfx_my_effect",
  "impactVFX":    "vfx_my_effect"
}
```

**Step 4: Push event from code (e.g., on impact):**
```cpp
m_vfxQueue->push(VFXEvent{
    .type     = VFXEventType::Spawn,
    .effectID = "vfx_my_effect",
    .position = impactPos,
    .direction = glm::vec3(0, 1, 0),
    .scale    = 1.0f
});
```

### 4.10 Example: Fireball VFX Full Flow

Ability JSON (`assets/abilities/fire_mage_fireball.json`):
```json
{
  "id": "fire_mage_fireball",
  "slot": "Q",
  "targeting": "SKILLSHOT",
  "castTime": 0.25,
  "castRange": 1100.0,
  "projectile": { "speed": 1200.0, "width": 60.0, "maxRange": 1100.0 },
  "castVFX": "vfx_fireball_cast",
  "projectileVFX": "vfx_fireball_projectile",
  "impactVFX": "vfx_fireball_explosion"
}
```

Projectile effect (`vfx_fireball_projectile.json`):
```json
{
  "id": "vfx_fireball_projectile",
  "maxParticles": 64,
  "emitRate": 80.0,
  "looping": true,
  "duration": 3.0,
  "lifetimeMin": 0.15,
  "lifetimeMax": 0.35,
  "gravity": 0.0,
  "colorOverLifetime": [
    {"time": 0.0, "color": [1.0, 0.8, 0.3, 1.0]},
    {"time": 1.0, "color": [1.0, 0.2, 0.0, 0.0]}
  ]
}
```

Impact effect (`vfx_fireball_explosion.json`):
```json
{
  "id": "vfx_fireball_explosion",
  "maxParticles": 256,
  "burstCount": 120.0,
  "looping": false,
  "duration": 0.8,
  "spreadAngle": 180.0,
  "gravity": 5.0,
  "colorOverLifetime": [
    {"time": 0.0, "color": [1.0, 0.9, 0.4, 1.0]},
    {"time": 0.3, "color": [1.0, 0.3, 0.0, 0.9]},
    {"time": 0.7, "color": [0.4, 0.1, 0.0, 0.5]},
    {"time": 1.0, "color": [0.1, 0.0, 0.0, 0.0]}
  ]
}
```

Execution flow:
1. **Input:** Player presses Q, targets ground at distance 500
2. **AbilitySystem** validates → emits `VFXEvent(Spawn, "vfx_fireball_cast", casterPos)`
3. **VFXRenderer** spawns cast particle effect at caster
4. **ProjectileSystem** spawns projectile mesh, moves it to target
5. **During projectile flight:** `VFXEvent(Move, vfxHandle, projectilePos)` each frame
6. **On impact:** `VFXEvent(Spawn, "vfx_fireball_explosion", impactPos)`
7. **Compute shader** simulates all particles; vertex shader billboards them

### 4.11 VFX Troubleshooting

| Issue | Checks |
|-------|--------|
| Particles not rendering | `ParticleSystem::isAlive()` returning false? Descriptor set binding 1 (atlas) valid? Pipeline depth write enabled? |
| VFX events dropped | Queue full (256 max)? Render thread slow, not draining? |
| Particle simulation wrong | `dt` correct in push constant? Gravity direction (should be -Y for downward)? |
| Fog not visible | `fogStart/fogEnd` in correct range? `fogDensity` too high? |

### 4.12 File Reference

| Path | Purpose | Key Functions |
|------|---------|----------------|
| `shaders/particle.vert` | Billboard expansion | SSBO read, camera-space expansion |
| `shaders/particle.frag` | Particle rendering | Atlas sample, alpha blend |
| `shaders/particle_sim.comp` | GPU simulation | Lifetime, gravity, integration, fade |
| `shaders/triangle.vert` | Model render | Skinning, UV/normal prep |
| `shaders/triangle.frag` | PBR + fog | Cook-Torrance, fog blending |
| `shaders/skinned.vert` | GPU skinning | Bone blending, normal xform |
| `shaders/debug.vert/frag` | Debug lines | Immediate-mode geometry |
| `shaders/grid.vert/frag` | World grid | Anti-aliased infinite plane |
| `shaders/click_indicator.vert/frag` | Click UI | Sprite atlas animation |
| `src/vfx/VFXTypes.h` | Data structures | GpuParticle, EmitterDef, VFXEvent |
| `src/vfx/VFXEventQueue.h` | SPSC queue | Lock-free ring buffer |
| `src/vfx/ParticleSystem.h/cpp` | Single emitter | SSBO, CPU emission, descriptor set |
| `src/vfx/VFXRenderer.h/cpp` | Master VFX | Pipelines, pool, per-frame flow |
| `src/renderer/ClickIndicatorRenderer.h/cpp` | Click animation | Sprite-based UI |
| `src/nav/DebugRenderer.h/cpp` | Debug visualization | Line/circle/AABB/sphere drawing |
| `src/renderer/Descriptors.h` | Uniform data | LightUBO, fog params, bone SSBO |
| `src/renderer/Renderer.h/cpp` | Main loop | Per-frame orchestration, command recording |
| `assets/vfx/*.json` | Particle configs | EmitterDef parameters, curves |
| `assets/abilities/*.json` | Ability definitions | Cooldown, VFX refs, scaling, effects |

---

## 5. Camera & Physics

### 5.1 IsometricCamera (`src/terrain/IsometricCamera.h/.cpp`)

MOBA-style isometric camera controller:

- **Projection:** Perspective, FOV 45°
- **Zoom range:** 15–50 units
- **Pitch:** 56° (fixed top-down-ish angle)
- **Follows character entity** (smooth pan)
- **Bounds:** Camera constrained to map bounds (0–200 XZ)

```cpp
class IsometricCamera {
public:
    glm::mat4 getViewMatrix() const;
    glm::mat4 getProjectionMatrix(float aspect) const;

    // Unproject screen coordinates to world-space ray
    std::pair<glm::vec3,glm::vec3> screenToWorldRay(
        glm::vec2 screenPos, float winW, float winH) const;

    void setTarget(glm::vec3 worldPos);
    void processScroll(float delta);   // Zoom in/out
    void setZoomRange(float minZ, float maxZ);
    void setBounds(glm::vec3 min, glm::vec3 max);
};
```

**Screen-to-World Raycasting:**
```cpp
// Takes screen coords + window dimensions
// Returns ray origin + direction in world space
// Uses glm::inverse(proj * view) to unproject NDC near/far points
// Handles Vulkan Y-flip in NDC conversion
```

### 5.2 Input→Camera→Movement Flow

1. **GLFW mouse button callback** → `InputManager::mouseButtonCallback()` → sets `m_rightClicked = true` + records screen position
2. **Main loop** → `m_input->wasRightClicked()` → returns true, resets flag, queries position
3. **`IsometricCamera::screenToWorldRay()`** → ray in world space
4. **Ray-plane intersection** at Y=0 (ground plane) → `hitPos`
5. **`character.targetPosition = hitPos`** + spawn click indicator
6. **`Scene::update()`** → moves character toward target at `moveSpeed * dt`

### 5.3 InputManager (`src/input/InputManager.h/.cpp`)

```cpp
class InputManager {
public:
    InputManager(GLFWwindow *window, Camera &camera);

    bool wasRightClicked();         // Movement command (consume-once)
    glm::vec2 getLastClickPos() const;
    bool wasLeftClicked();          // Selection/marquee
    glm::vec2 getLastLeftClickPos() const;
    bool isRightMouseDown() const;
    bool isLeftMouseDown() const;

    glm::vec2 getMousePos() const;  // Current mouse position (screen space)
    float consumeScrollDelta();     // Camera zoom accumulation
    void setCaptureEnabled(bool);   // false = MOBA mode (no FPS cursor capture)
    bool isCursorCaptured() const;
    void update(float deltaTime);

private:
    bool m_rightClicked = false;
    glm::vec2 m_rightClickPos{0.0f};
    bool m_leftClicked = false;
    glm::vec2 m_leftClickPos{0.0f};
    float m_scrollDelta = 0.0f;

    static void keyCallback(GLFWwindow*, int, int, int, int);
    static void mouseCallback(GLFWwindow*, double, double);
    static void scrollCallback(GLFWwindow*, double, double);
    static void mouseButtonCallback(GLFWwindow*, int, int, int);
};
```

Key design:
- **MOBA Mode:** `setCaptureEnabled(false)` — right-click triggers movement command
- **One-shot events:** `wasRightClicked()` returns true once, then automatically resets
- **Scroll accumulation:** `m_scrollDelta` tracks scroll wheel delta for camera zoom

### 5.4 Smooth Rotation & Quaternion Math

Character facing interpolation uses Spherical Linear Interpolation (Slerp):

```cpp
// Target quaternion from velocity direction
float targetYaw = std::atan2(velocityX, velocityZ);
float halfYaw   = targetYaw * 0.5f;
Quaternion targetRot = { 0.0f, std::sin(halfYaw), 0.0f, std::cos(halfYaw) };

// Constant angular velocity Slerp
float cosOmega   = Quaternion::Dot(currentRot, targetRot);
if (cosOmega < 0.0f) { targetRot = -targetRot; cosOmega = -cosOmega; }
cosOmega = std::clamp(cosOmega, -1.0f, 1.0f);
float totalAngle = std::acos(cosOmega);

if (totalAngle > 0.001f) {
    float t = (turnSpeed * deltaTime) / totalAngle;
    t = std::clamp(t, 0.0f, 1.0f);
    currentRot = Slerp(currentRot, targetRot, t);
} else {
    currentRot = targetRot;  // Snap to final rotation (eliminate micro-jitter)
}
```

**Why quaternions over Euler angles:**

| Paradigm | Interpolation Quality | Memory | Overhead | Gimbal Lock |
|----------|----------------------|--------|----------|-------------|
| Euler Angles | Poor (non-linear, erratic) | 12B | Low | High |
| Rotation Matrices | Poor (re-orthogonalization needed) | 36B | High | None |
| Unit Quaternions | **Excellent** (smooth, constant via Slerp) | 16B | Moderate | None |

### 5.5 Character Movement System

`Scene::update()` movement:
```cpp
// Move toward target at moveSpeed * deltaTime
// Rotate Y axis using atan2 to face movement direction (lerp for smooth turning)
// Snap Y position to terrain height via TerrainSystem::GetHeightAt()
// Stop when within 0.1 units of target
```

`CharacterComponent` state:
```cpp
struct CharacterComponent {
    glm::vec3 targetPosition{0.0f};         // Destination from right-click
    float     moveSpeed = 6.0f;             // Units per second
    bool      hasTarget = false;
    glm::quat currentFacing{1.0f,0,0,0};   // Smooth rotation state
    float     currentSpeed = 0.0f;          // 0 to moveSpeed (acceleration)
};
```

### 5.6 Physics Integration

- **Internal deterministic physics engine** (`src/physics/PhysicsEngine.h/.cpp`)
- **Fixed-point math** (`Fixed64`, `Fixed32`) for deterministic cross-platform simulation
- **Spatial hash** for neighbor queries — O(1) per entity
- **Soft-body separation** for minion-minion interactions (steering behaviors)
- Separation force applied if within separation radius (e.g., 50 units); blended with path-following

### 5.7 Coordinate System

- **Right-handed, Y-up**
- **+X** = right, **+Y** = up, **+Z** = backward (entity local forward = −Z)
- GLM used with `GLM_FORCE_DEPTH_ZERO_TO_ONE` for Vulkan depth range
- Map bounds: `(0,0,0)` to `(200, maxHeight, 200)`, center at `(100, 0, 100)`
- Team 1 (Blue) base: near `(0, 0, 0)` | Team 2 (Red) base: near `(200, 0, 200)`
