# Glory Game Engine - Rendering & Shader System Deep Dive

## Executive Summary

The Glory engine features a **GPU-accelerated particle VFX system** with lock-free thread communication, **modern PBR rendering**, **immediate-mode debug visualization**, and **distance fog**. The architecture separates game logic (abilities) from rendering through a SPSC event queue, eliminating shared mutable state between threads.

---

## 1. SHADER FILES INVENTORY

All shader source files are in `/Users/donkey/Development/1/Glory/shaders/` and compiled to `.spv` (SPIR-V) bytecode.

### 1.1 Particle System Shaders

#### `particle.vert` (Billboard Vertex Shader)
- **Purpose**: Expands GPU-simulated particles into camera-facing billboards
- **Key Features**:
  - **No vertex buffer** — reads particle state directly from SSBO
  - 6 vertices per particle (2 triangles = 1 quad)
  - Dead particles output `gl_Position.z = 2.0` (clipped)
  - Spherical billboard expansion using camera right/up vectors
  
- **Input**:
  - Binding 0: ParticleSSBO (readonly) — `struct Particle` (64 bytes each)
  - Push constants (96 bytes):
    - `viewProj` (mat4, 64 bytes)
    - `camRight` (vec4, 16 bytes)
    - `camUp` (vec4, 16 bytes)

- **Output**:
  - `gl_Position` (projected billboard quad vertex)
  - `fragColor` (particle color, fades over lifetime)
  - `fragUV` (atlas UV coordinates)

- **Algorithm**:
  ```glsl
  pi = gl_VertexIndex / 6    // particle index
  vi = gl_VertexIndex % 6    // quad vertex index
  
  // Expand in camera space
  worldPos = particle.pos + camRight * offset.x * size + camUp * offset.y * size
  ```

#### `particle.frag` (Fragment Shader)
- **Purpose**: Samples atlas texture and applies per-particle color
- **Blending**: Alpha-blended, no depth write (depth test enabled)
- **Early Discard**: Skips transparent pixels < 0.01 alpha
- **Input**:
  - Binding 1: Combined image sampler (particle atlas texture)
  - `fragColor` (per-particle color)
  - `fragUV` (atlas UV)

#### `particle_sim.comp` (GPU Particle Simulation)
- **Scope**: Local size 64 threads (warp/wavefront-aligned)
- **Purpose**: Updates particle physics on GPU (no CPU→GPU round-trip)

- **Key Functions**:
  1. Skip dead particles (params.w < 0.5)
  2. Decrement lifetime: `posLife.w -= dt`
  3. Increment age: `velAge.w += dt`
  4. If lifetime ≤ 0 → mark dead (params.w = 0)
  5. Apply gravity: `velAge.y -= gravity * dt`
  6. Euler integrate: `posLife.xyz += velAge.xyz * dt`
  7. Fade alpha: `color.a = lifeFrac` (linear over lifetime)

- **Push Constants**:
  - `dt` (float) — frame delta time
  - `gravity` (float) — downward acceleration (m/s²)
  - `count` (uint32_t) — particle count
  - `_pad` (float) — alignment

### 1.2 Model Rendering Shaders

#### `triangle.vert` (Standard Model Vertex Shader)
- **Purpose**: Forward pass skinned/static model rendering
- **Attributes**:
  - Position, Color, Normal, TexCoord (per-vertex)
  - Model, Normal matrix (per-instance)
  - Material params (tint, shininess, metallic, roughness)
  - Texture indices (bindless lookup)

- **Output**: World position, normal, light-space position

#### `triangle.frag` (PBR Fragment Shader)
- **Comprehensive rendering** with:
  - **Cook-Torrance BRDF** (GGX normal distribution, Schlick-GGX geometry)
  - **Fresnel-Schlick** approximation
  - **Bindless texture array** (up to 64 textures)
  - **Shadow mapping** (16-sample Poisson disk soft shadows)
  - **Normal mapping** via cotangent-frame TBN
  - **Subsurface scattering** (wrap lighting for skin)
  - **Fresnel rim lighting**
  - **Thin-film iridescence** (metallic surfaces)
  - **Clearcoat layer** (car paint effect)
  - **Emissive glow**
  - **Micro AO** (curvature-based)
  - **Distance fog** (exponential with color blending)

- **Fog Implementation** (lines 281-285):
  ```glsl
  float fogDist = length(viewPos - worldPos);
  float fogFactor = exp(-fogDensity * max(fogDist - fogStart, 0.0));
  fogFactor = clamp(fogFactor, 0.0, 1.0);
  result = mix(fogColor, result, fogFactor);
  ```

### 1.3 Skinned Model Shader

#### `skinned.vert` (GPU Skinning Vertex Shader)
- **Purpose**: Real-time skeletal animation (GPU-driven blend shapes)
- **Features**:
  - Up to 4 bone influences per vertex (weights sum to 1.0)
  - Joint indices local to entity (offset by `boneBaseIndex` push constant)
  - Large shared bone SSBO (MAX_CHARS × MAX_BONES × mat4)
  - Parallel normal transformation

- **Key Lines** (48-68):
  ```glsl
  uint base = boneBaseIndex;
  skinMat = bones[base + joints.x] * weights.x 
          + bones[base + joints.y] * weights.y
          + bones[base + joints.z] * weights.z
          + bones[base + joints.w] * weights.w;
  
  skinnedPos = skinMat * position;  // bind-pose → skinned local
  worldPos = model * skinnedPos;    // → world space
  ```

### 1.4 Debug/UI Shaders

#### `debug.vert` / `debug.frag` (Debug Line Renderer)
- **Purpose**: Immediate-mode debug visualization (lines, circles, AABBs, spheres)
- **Usage**: Navigation grid, collision volumes, pathfinding viz
- **Input**: Position (vec3), Color (vec4) per vertex
- **Topology**: Line list

#### `grid.vert` / `grid.frag` (Infinite Grid Shader)
- **Purpose**: Fullscreen world-space grid at Y=gridY
- **Features**:
  - Fine grid (1 unit) + coarse grid (5 units)
  - Distance fade (15–40 unit range)
  - Axis highlighting (red X, blue Z)
  - Anti-aliased grid lines via `fwidth()`

#### `click_indicator.vert` / `click_indicator.frag` (Click Animation)
- **Purpose**: Click feedback UI overlay
- **Features**:
  - Sprite atlas with frame indexing
  - Dynamic tinting
  - Push constant animation frame selection
  - Vertex-driven position scaling

---

## 2. VFX SYSTEM ARCHITECTURE

### 2.1 Core Types & Data Structures

**Location**: `/Users/donkey/Development/1/Glory/src/vfx/VFXTypes.h`

#### GpuParticle (64 bytes, SSBO-aligned)
```cpp
struct alignas(16) GpuParticle {
    glm::vec4 posLife;   // xyz = world pos,  w = remaining lifetime (s)
    glm::vec4 velAge;    // xyz = velocity (m/s), w = age (s)
    glm::vec4 color;     // rgba (alpha fades to 0 at death)
    glm::vec4 params;    // x=size, y=rotation, z=atlasFrame, w=active(1)/dead(0)
};
static_assert(sizeof(GpuParticle) == 64);  // Must be exactly 64 bytes
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
    float        duration        = 1.5f;       // emitter lifetime
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
    Spawn,         // Create new effect
    Destroy,       // Force-stop early
    Move,          // Teleport to new position
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

### 2.2 SPSC Event Queue

**Location**: `/Users/donkey/Development/1/Glory/src/vfx/VFXEventQueue.h`

- **Type**: Lock-free Single-Producer Single-Consumer ring buffer
- **Capacity**: 256 slots (power-of-two)
- **Memory Ordering**:
  - Producer: `push()` uses `memory_order_release` on head
  - Consumer: `pop()` uses `memory_order_release` on tail
  - Loads use `memory_order_acquire`
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

**Why SPSC?**
- Game thread produces ability events
- Render thread consumes and spawns particle systems
- **Zero copying** of particles (shared SSBO, GPU-simulated)
- **No mutex lock** (atomic word swaps only)

### 2.3 ParticleSystem (Single Active Effect)

**Location**: `/Users/donkey/Development/1/Glory/src/vfx/ParticleSystem.h/.cpp`

Manages **one active particle emitter** instance:

```cpp
class ParticleSystem {
    // GPU SSBO (persistent CPU mapping via VMA_MEMORY_USAGE_CPU_TO_GPU)
    Buffer m_ssboBuffer;
    GpuParticle* m_particles;  // mapped pointer for CPU writes

    // Emitter state
    glm::vec3 m_position;
    glm::vec3 m_direction;
    float m_scale, m_timeAlive, m_duration, m_emitAccum;
    bool m_looping, m_stopped;

    // CPU emission (called each frame)
    void update(float dt);
    
    // Private helper
    void spawnParticle();  // Find dead slot, randomize spawn params

    // GPU resources
    VkDescriptorSet m_descSet;  // Binding 0=SSBO, 1=atlas
};
```

**Particle Spawning** (CPU, CPU_TO_GPU memory):
1. Find first dead slot (`params.w < 0.5`)
2. Randomize velocity inside cone around direction (using spherical coords)
3. Randomize lifetime, size, initial color
4. Write to SSBO at mapped pointer (no GPU sync needed)
5. Repeat up to `emitRate * dt` times per frame

**Important**: The SSBO is allocated with `VMA_MEMORY_USAGE_CPU_TO_GPU` so:
- On Apple Silicon (unified memory): **zero-copy** via MoltenVK
- On discrete GPUs: lives in BAR/ReBAR memory (PCIe bar, fast CPU↔GPU)

### 2.4 VFXRenderer (Master Orchestrator)

**Location**: `/Users/donkey/Development/1/Glory/src/vfx/VFXRenderer.h/.cpp`

Owns pipelines, manages emitter pool (up to 32 concurrent), loads JSON configs:

```cpp
class VFXRenderer {
    // Pools & layouts
    VkDescriptorPool m_descPool;
    VkDescriptorSetLayout m_descLayout;

    // Pipelines
    VkPipeline m_computePipeline;   // particle_sim.comp
    VkPipeline m_renderPipeline;    // particle.vert/frag

    // Per-frame flow (called by Renderer::drawFrame())
    void processQueue(VFXEventQueue& queue);           // Drain events
    void update(float dt);                             // Emit particles
    void dispatchCompute(VkCommandBuffer cmd);         // GPU simulation
    void barrierComputeToGraphics(VkCommandBuffer cmd); // Synchronize
    void render(VkCommandBuffer cmd, ...);             // Draw billboards

    // Registry
    void registerEmitter(EmitterDef def);
    void loadEmitterDirectory(const std::string& path);  // JSON hot-load

private:
    std::vector<ParticleSystem> m_effects;  // Active emitters
    std::unordered_map<std::string, Texture*> m_atlasCache;  // Loaded textures
};
```

**Per-Frame Execution Order**:
1. **Game thread**: `AbilitySystem::update()` → fires ability → `queue.push(VFXEvent)`
2. **Render thread** (before present):
   - `VFXRenderer::processQueue(queue)` — handle Spawn/Destroy/Move
   - `VFXRenderer::update(dt)` — CPU emission bookkeeping
   - **(Outside render pass)**:
     - `VFXRenderer::dispatchCompute(cmd)` — GPU particle simulation
     - `VFXRenderer::barrierComputeToGraphics(cmd)` — sync
   - **(Inside render pass)**:
     - `VFXRenderer::render(cmd, viewProj, camRight, camUp)` — draw

### 2.5 Descriptor Set Layout

Both compute and graphics pipelines share the **same** layout:

| Binding | Type | Stages | Purpose |
|---------|------|--------|---------|
| 0 | `STORAGE_BUFFER` | Compute (RW) + Vertex (R) | Particle SSBO |
| 1 | `COMBINED_IMAGE_SAMPLER` | Fragment (R) | Particle atlas texture |

One descriptor set per active emitter, allocated from shared pool (MAX_CONCURRENT_EMITTERS=32).

---

## 3. VFX EXAMPLE: FIREBALL ABILITY

### 3.1 Ability JSON Definition

**File**: `/Users/donkey/Development/1/Glory/assets/abilities/fire_mage_fireball.json`

```json
{
  "id": "fire_mage_fireball",
  "slot": "Q",
  "targeting": "SKILLSHOT",
  "castTime": 0.25,
  "castRange": 1100.0,
  "projectile": {
    "speed": 1200.0,
    "width": 60.0,
    "maxRange": 1100.0
  },
  "castVFX": "vfx_fireball_cast",
  "projectileVFX": "vfx_fireball_projectile",
  "impactVFX": "vfx_fireball_explosion"
}
```

### 3.2 Particle Effect JSONs

**Cast Effect** (`vfx_fireball_cast.json`):
- Burst of particles at caster location
- High alpha, narrow spread, quick fade

**Projectile Effect** (`vfx_fireball_projectile.json`):
```json
{
  "id": "vfx_fireball_projectile",
  "maxParticles": 64,
  "emitRate": 80.0,
  "looping": true,
  "duration": 3.0,
  "lifetimeMin": 0.15,
  "lifetimeMax": 0.35,
  "initialSpeedMin": 0.5,
  "initialSpeedMax": 2.0,
  "sizeMin": 0.2,
  "sizeMax": 0.4,
  "spreadAngle": 80.0,
  "gravity": 0.0,
  "colorOverLifetime": [
    {"time": 0.0, "color": [1.0, 0.8, 0.3, 1.0]},
    {"time": 1.0, "color": [1.0, 0.2, 0.0, 0.0]}
  ]
}
```

**Impact Effect** (`vfx_fireball_explosion.json`):
```json
{
  "id": "vfx_fireball_explosion",
  "maxParticles": 256,
  "burstCount": 120.0,      // Instant spawn
  "looping": false,
  "duration": 0.8,
  "lifetimeMin": 0.5,
  "lifetimeMax": 1.2,
  "initialSpeedMin": 3.0,
  "initialSpeedMax": 12.0,
  "sizeMin": 0.3,
  "sizeMax": 0.9,
  "spreadAngle": 180.0,      // All directions
  "gravity": 5.0,            // Falls under gravity
  "colorOverLifetime": [
    {"time": 0.0, "color": [1.0, 0.9, 0.4, 1.0]},
    {"time": 0.3, "color": [1.0, 0.3, 0.0, 0.9]},
    {"time": 0.7, "color": [0.4, 0.1, 0.0, 0.5]},
    {"time": 1.0, "color": [0.1, 0.0, 0.0, 0.0]}
  ]
}
```

### 3.3 Execution Flow

1. **Input**: Player presses Q, targets ground at distance 500
2. **AbilitySystem** validates → emits `VFXEvent(Spawn, "vfx_fireball_cast", casterPos)`
3. **VFXRenderer** spawns cast particle effect at caster
4. **ProjectileSystem** (separate) spawns projectile mesh, moves it to target
5. **During projectile flight**: `VFXEvent(Move, vfxHandle, projectilePos)` each frame
6. **On impact**: `VFXEvent(Spawn, "vfx_fireball_explosion", impactPos)`
7. **Compute shader** simulates all particles, vertex shader billboards them

---

## 4. MAIN RENDERER INTEGRATION

**Location**: `/Users/donkey/Development/1/Glory/src/renderer/Renderer.h/.cpp`

### 4.1 Renderer Class Structure

```cpp
class Renderer {
private:
    // Vulkan core
    std::unique_ptr<Context>     m_context;
    std::unique_ptr<Device>      m_device;
    std::unique_ptr<Swapchain>   m_swapchain;
    std::unique_ptr<Sync>        m_sync;      // Fences, semaphores
    std::unique_ptr<Descriptors> m_descriptors;
    std::unique_ptr<Pipeline>    m_pipeline;  // Forward renderpass + framebuffers

    // Render overlays
    std::unique_ptr<ClickIndicatorRenderer> m_clickIndicatorRenderer;
    DebugRenderer m_debugRenderer;

    // VFX & Ability
    std::unique_ptr<VFXEventQueue> m_vfxQueue;
    std::unique_ptr<VFXRenderer>   m_vfxRenderer;
    std::unique_ptr<AbilitySystem> m_abilitySystem;

    // Scene & Camera
    Scene m_scene;
    IsometricCamera m_isoCam;
    std::unique_ptr<InputManager> m_input;

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, float dt);
};
```

### 4.2 drawFrame() Order (Main Render Loop)

Located in `Renderer::drawFrame()` (~lines 107–250):

```cpp
void Renderer::drawFrame() {
    // 1. Update delta time
    float dt = currentTime - m_lastFrameTime;
    m_currentDt = dt;

    // 2. Flush VFX events & update CPU emitters
    m_vfxRenderer->processQueue(*m_vfxQueue);
    m_vfxRenderer->update(dt);

    // 3. Game logic (abilities use VFX queue)
    m_abilitySystem->update(m_scene.getRegistry(), dt);

    // 4. Wait for GPU, acquire swapchain image
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkAcquireNextImageKHR(dev, swapchain, ...);

    // 5. Record command buffer
    recordCommandBuffer(cmd, imageIndex, dt);

    // 6. Submit → present
    vkQueueSubmit(...);
    vkQueuePresentKHR(...);
}
```

### 4.3 recordCommandBuffer() Order (Command Submission)

```cpp
void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, float dt) {
    vkCmdBeginRenderPass(cmd, ...);

    // ── Opaque scene render ────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
    // Draw minions, structures, etc. with depth write + test
    // ... instanced draws ...

    // ── VFX (GPU particle simulation) ────────────────
    m_vfxRenderer->dispatchCompute(cmd);         // Outside render pass? YES
    m_vfxRenderer->barrierComputeToGraphics(cmd);

    // Actually, VFX rendering happens INSIDE the render pass
    // (after opaque geometry, before UI)

    // ── UI Overlays ────────────────────────────
    m_clickIndicatorRenderer->render(cmd, viewProj, ...);
    
    // ── Debug visualization ───────────────────
    m_debugRenderer.render(cmd, viewProj);

    // ── Grid ─────────────────────────────────
    if (m_showGrid) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
        vkCmdPushConstants(cmd, m_gridPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, ...);
        vkCmdDraw(cmd, 6, 1, 0, 0);  // 6 vertices = fullscreen quad
    }

    vkCmdEndRenderPass(cmd);
}
```

**Note**: The VFX compute dispatch should occur **outside** the render pass (before `vkCmdBeginRenderPass`), but the particle billboard render happens **inside** (after opaque geometry, before UI).

---

## 5. DEBUG UI & OVERLAY RENDERING

### 5.1 DebugRenderer (Immediate-Mode Line Renderer)

**Location**: `/Users/donkey/Development/1/Glory/src/nav/DebugRenderer.h/.cpp`

Provides **immediate-mode drawing** of geometric primitives:

```cpp
class DebugRenderer {
public:
    void init(const Device& device, VkRenderPass renderPass);

    // ── Drawing API (call during update) ──────────────
    void drawLine(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color);
    void drawCircle(const glm::vec3& center, float radius, const glm::vec4& color, int segments=32);
    void drawAABB(const glm::vec3& min, const glm::vec3& max, const glm::vec4& color);
    void drawSphere(const glm::vec3& center, float radius, const glm::vec4& color, int segments=8);

    // ── Render (call during command recording) ───────────────
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj);

    void clear();  // Called at frame start
};
```

**Implementation Details**:
- **CPU-side vertex buffer** (dynamic, CPU_TO_GPU)
- **Single VkPipeline** (debug.vert/debug.frag)
- **Line topology** (not triangle list)
- **Auto-grow** buffer if capacity exceeded
- **Per-frame clear** + push new vertices

**Use Case**: Draw navigation grid, collision volumes, pathfinding debug info

### 5.2 ClickIndicatorRenderer

**Location**: `/Users/donkey/Development/1/Glory/src/renderer/ClickIndicatorRenderer.h/.cpp`

Renders **click feedback animations** (where player clicked):

```cpp
class ClickIndicatorRenderer {
public:
    ClickIndicatorRenderer(const Device& device, VkRenderPass renderPass);

    void render(VkCommandBuffer cmd,
                const glm::mat4& viewProj,
                const glm::vec3& center,
                float t,  // Animation time (0–1)
                float size,
                const glm::vec4& tint = glm::vec4(1.0f));

private:
    std::unique_ptr<Texture> m_texture;  // Sprite atlas
    Buffer m_vertexBuffer;               // Quad mesh
    VkPipeline m_pipeline;
};
```

**Pipeline**:
- Vertex buffer: one quad (-0.5..0.5 in XY)
- **Shader drives atlas** via push constants:
  - `frameIndex` — which frame in sprite sheet
  - `gridCount` — frames per row
  - `tint` — color multiplication
  - `viewProj` — camera matrix

**Shader** (click_indicator.vert/frag):
```glsl
// Calculate UV from frameIndex + gridCount
int row = frameIndex / gridCount;
int col = frameIndex % gridCount;
float step = 1.0 / gridCount;
outUV = (vec2(col, row) + inUV) * step;
```

---

## 6. FOG RENDERING

### 6.1 Fog Parameters

**Location**: `/Users/donkey/Development/1/Glory/src/renderer/Descriptors.h` (lines 29–40)

```cpp
struct LightUBO {
    // ... lights ...
    alignas(16) glm::vec3 fogColor{0.6f, 0.65f, 0.75f};  // Blue-gray
    alignas(4)  float     fogDensity       = 0.03f;
    alignas(4)  float     fogStart         = 5.0f;        // meters
    alignas(4)  float     fogEnd           = 50.0f;       // meters
};
```

### 6.2 Fog Implementation (Fragment Shader)

**Location**: `/Users/donkey/Development/1/Glory/shaders/triangle.frag` (lines 281–285)

```glsl
// Exponential distance fog with near/far falloff
float fogDist = length(lightData.viewPos - fragWorldPos);
float fogFactor = exp(-lightData.fogDensity * max(fogDist - fogStart, 0.0));
fogFactor = clamp(fogFactor, 0.0, 1.0);
result = mix(lightData.fogColor, result, fogFactor);
```

**Formula**:
- `fogFactor = exp(-density * (distance - fogStart))`
  - At fogStart: minimal fog
  - At fogEnd: heavy fog (density controls falloff rate)
  - Exponential decay for smooth depth progression

### 6.3 How to Adjust Fog

**In C++ code** (`Renderer.cpp` during frame update):
```cpp
LightUBO lightData{};
lightData.fogColor = glm::vec3(0.6f, 0.65f, 0.75f);  // RGB color
lightData.fogDensity = 0.03f;                        // Decrease for lighter fog
lightData.fogStart = 5.0f;                           // Near edge (meters)
lightData.fogEnd = 50.0f;                            // Far edge (meters)
m_descriptors->updateLightBuffer(frameIndex, lightData);
```

**Current Settings** (defaults in LightUBO):
- **Color**: Blueish-gray (0.6, 0.65, 0.75)
- **Density**: 0.03 (moderate fog)
- **Start**: 5m (light fog begins here)
- **End**: 50m (heavy fog by here)

---

## 7. SHADER INFRASTRUCTURE FOR NEW VFX

### 7.1 Adding a New Particle Effect

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

**Step 2: Load in game code**:
```cpp
m_vfxRenderer->loadEmitterDirectory(std::string(ASSET_DIR) + "vfx/");
// Or register manually:
EmitterDef def{...};
m_vfxRenderer->registerEmitter(def);
```

**Step 3: Emit from ability system** (in ability JSON):
```json
{
  "castVFX": "vfx_my_effect",
  "projectileVFX": "vfx_my_effect",
  "impactVFX": "vfx_my_effect"
}
```

### 7.2 Customizing Particle Shaders

**Current Rendering** (`particle.vert/frag`):
- Simple billboard expansion (camera-facing)
- Atlas texture sampling with color multiplication
- Linear alpha fade

**To Add New Effects**, modify shaders:

1. **Add rotation**: Use `params.y` (currently unused) to rotate billboard
   ```glsl
   // In particle.vert
   float angle = p.params.y;
   mat2 rot = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
   offset = rot * offset;
   ```

2. **Add distortion**: Sample normal map, perturb UV
   ```glsl
   // In particle.frag
   vec2 distort = texture(normalSampler, fragUV).xy * 2.0 - 1.0;
   vec2 finalUV = fragUV + distort * 0.1;
   ```

3. **Add emission glow**: Use `params.w` to modulate emissive
   ```glsl
   outColor = tex * fragColor + tex * 0.5;  // Additive glow
   ```

4. **Add soft particles** (depth fade at surface):
   ```glsl
   float sceneDepth = texture(depthBuffer, screenCoords).r;
   float particleDepth = gl_FragCoord.z;
   float fade = smoothstep(0.0, softnessRange, sceneDepth - particleDepth);
   outColor.a *= fade;
   ```

### 7.3 Adding Custom Compute Shader Effects

The particle compute shader (`particle_sim.comp`) can be extended:

**Current Features**:
- Lifetime decay
- Gravity application
- Euler integration
- Linear alpha fade

**Extensions** (example):
```glsl
// Add wind force
vec3 wind = vec3(sin(p.velAge.w) * 0.5, 0.0, 0.0);
p.velAge.xyz += wind * dt;

// Add drag / air resistance
p.velAge.xyz *= 0.98;  // 2% damping per frame

// Add rotation
float angularVel = frand(-1.0, 1.0);
p.params.y += angularVel * dt;

// Add color interpolation from curve
// (requires separate color curve SSBO)
```

---

## 8. ADDING NEW VFX (ATTACK SLASHES, SHIELDS, PROJECTILES)

### 8.1 Attack Slash VFX (Using Particles + Geometry)

**Approach 1: Pure Particles**
- Use a slash atlas texture (pre-rendered animated slash images)
- Emit particles in a line perpendicular to swing direction
- High initial speed, quick fade

**Approach 2: Hybrid (Recommended)**
- Static mesh slash geometry (pre-modeled slash shape)
- Spawn particle effects around it for "energy" feedback
- ParticleSystem handles the secondary glow/spark effect

**Implementation**:
```cpp
// In AbilitySystem::executeAbility()
// 1. Spawn geometry mesh (separate from VFX)
auto slashEntity = m_scene.addEntity();
slashEntity.addComponent<MeshComponent>(loadMesh("models/slash.glb"));
slashEntity.addComponent<TransformComponent>(worldPos, rotation);

// 2. Emit particle effect for glow
m_vfxQueue->push(VFXEvent{
    .type = VFXEventType::Spawn,
    .effectID = "vfx_attack_slash_glow",
    .position = worldPos,
    .direction = attackDirection,
    .scale = 1.5f
});

// 3. Schedule mesh deletion after 0.3s (via future/callback)
```

### 8.2 Shield VFX (Protective Overlay)

**Approach**: Additive blend particles expanding outward

**JSON Configuration** (`vfx_shield.json`):
```json
{
  "id": "vfx_shield",
  "maxParticles": 64,
  "emitRate": 0.0,
  "burstCount": 30.0,
  "looping": false,
  "duration": 0.5,
  "lifetimeMin": 0.4,
  "lifetimeMax": 0.5,
  "initialSpeedMin": 3.0,
  "initialSpeedMax": 6.0,
  "sizeMin": 0.5,
  "sizeMax": 1.2,
  "spreadAngle": 180.0,
  "gravity": 0.0,
  "colorOverLifetime": [
    {"time": 0.0, "color": [0.3, 0.7, 1.0, 0.8]},
    {"time": 1.0, "color": [0.1, 0.3, 1.0, 0.0]}
  ]
}
```

**Blending Variant**: To enable additive blending instead of alpha blend:
```cpp
// Modify VFXRenderer::createRenderPipeline()
cbA.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
cbA.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;  // Additive instead of ONE_MINUS_SRC_ALPHA
```

### 8.3 Projectile VFX (Moving Through Space)

**Three-Stage System**:

1. **Cast VFX** (at caster position, instant)
   - Burst of particles
   - "Preparing" animation

2. **Projectile VFX** (moves with projectile)
   - Continuous emission as projectile flies
   - Attached to projectile position via `Move` events
   
   ```cpp
   // Each frame during projectile flight:
   m_vfxQueue->push(VFXEvent{
       .type = VFXEventType::Move,
       .handle = vfxHandle,
       .position = projectileCurrentPos
   });
   ```

3. **Impact VFX** (at target, on collision)
   - Burst of particles
   - "Explosion" animation

**Code Flow**:
```cpp
void AbilitySystem::executeAbility(entt::entity caster, AbilityInstance& inst) {
    // 1. Cast VFX
    emitVFX("vfx_projectile_cast", casterPos, direction, 1.0f);

    // 2. Spawn projectile system entity
    auto projectile = m_scene.addEntity();
    projectile.addComponent<ProjectileComponent>(...);
    projectile.addComponent<VFXAttachmentComponent>("vfx_projectile_flight", vfxHandle);

    // 3. During ProjectileSystem::update() → send Move events
    // 4. On collision → emit impact VFX + destroy projectile
    emitVFX("vfx_projectile_impact", impactPos, ...);
}
```

---

## 9. RENDERING DEBUG UI OVERLAYS

### 9.1 Using DebugRenderer

**Example: Draw pathfinding grid**:
```cpp
// In Renderer::recordCommandBuffer()
if (m_showDebug) {
    m_debugRenderer.clear();
    
    // Draw path waypoints
    for (const auto& wp : m_navGrid.getWaypoints()) {
        m_debugRenderer.drawCircle(wp.pos, 0.5f, glm::vec4(0, 1, 0, 1), 16);
    }
    
    // Draw collision volumes
    for (const auto& npc : m_scene.getNPCs()) {
        auto* trans = m_scene.getComponent<TransformComponent>(npc);
        auto* coll = m_scene.getComponent<CollisionComponent>(npc);
        m_debugRenderer.drawSphere(trans->position, coll->radius,
                                   glm::vec4(1, 0, 0, 1), 8);
    }

    // Render all accumulated vertices
    m_debugRenderer.render(cmd, viewProj);
}
```

### 9.2 Creating Custom UI Overlay

**Approach 1: Use ClickIndicatorRenderer** (sprite-based)
- Fast & flexible for small UI elements
- Limited to textured quads

**Approach 2: Create ImGui Integration** (planned)
- Modern immediate-mode UI library
- Would require separate ImGui backend for Vulkan
- Good for debug stats, inventory screens

**Approach 3: Debug Geometry** (lines + text)
- Use DebugRenderer for wireframe overlays
- Add text via texture-rendered billboard (future)

### 9.3 Example: Health Bar Overlay

```cpp
// Approach: Draw debug quad + health bar using geometry

void drawHealthBar(glm::vec3 worldPos, float health, float maxHealth) {
    // Background bar (dark)
    float barWidth = 1.0f;
    float barHeight = 0.1f;
    glm::vec3 barMin = worldPos + glm::vec3(-barWidth/2, 2.0f, 0);
    glm::vec3 barMax = barMin + glm::vec3(barWidth, barHeight, 0);
    m_debugRenderer.drawAABB(barMin, barMax, glm::vec4(0, 0, 0, 1));

    // Health bar (green)
    float healthFrac = glm::clamp(health / maxHealth, 0.0f, 1.0f);
    glm::vec3 healthMax = barMin + glm::vec3(barWidth * healthFrac, barHeight, 0);
    m_debugRenderer.drawAABB(barMin, healthMax, glm::vec4(0, 1, 0, 1));
}
```

---

## 10. INTEGRATION POINTS & HOW TO EXTEND

### 10.1 Adding New Ability with VFX

**Files to modify**:

1. **Create ability JSON**: `assets/abilities/my_ability.json`
2. **Create VFX JSONs**: `assets/vfx/vfx_my_cast.json`, etc.
3. **Ability texture atlas**: `assets/textures/particles/my_effect.png`
4. **Game code** (`src/ability/AbilitySystem.cpp`):
   ```cpp
   // In executeAbility(), handle custom logic if needed
   // But most data is JSON-driven
   ```

### 10.2 Adding New Particle Simulation Feature

**To extend compute shader** (`particle_sim.comp`):

```glsl
// Example: add wind force
layout(push_constant) uniform SimPC {
    float    dt;
    float    gravity;
    uint     count;
    float    windStrength;  // New parameter
} pc;

// In main():
vec3 wind = vec3(sin(p.posLife.x + time) * pc.windStrength, 0, 0);
p.velAge.xyz += wind * pc.dt;
```

Then pass via VFXRenderer push constants:
```cpp
// In VFXRenderer::dispatchCompute()
struct SimPC {
    float dt;
    float gravity;
    uint32_t count;
    float windStrength;  // New
} pc{...};
```

### 10.3 Adding Fog Control UI

**Example: Debug menu to adjust fog**:
```cpp
// In Renderer::drawFrame() or input handler
if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS) {
    LightUBO light = m_descriptors->getCurrentLightData();
    light.fogDensity += 0.01f;
    m_descriptors->updateLightBuffer(frameIndex, light);
}
```

### 10.4 Planned: ImGui Integration

The HUD system is minimal (just a stub). To add ImGui:

1. **Add ImGui Vulkan backend** to CMakeLists.txt
2. **Initialize ImGui** in Renderer constructor
3. **Create ImGui render pipeline** (similar to DebugRenderer)
4. **Call ImGui::NewFrame()** at start of drawFrame()
5. **Render ImGui context** at end of render pass

---

## 11. KEY PERFORMANCE CONSIDERATIONS

### 11.1 GPU Particle Simulation Benefits

- **Zero GPU→CPU sync** (GPU writes & reads same SSBO)
- **Asynchronous simulation** (compute runs while CPU prepares next frame)
- **Scalable** to thousands of particles (limited by SSBO size + compute threads)
- **Reduced CPU overhead** (no particle update loop)

### 11.2 Memory Efficiency

| Component | Memory | Notes |
|-----------|--------|-------|
| GpuParticle | 64 bytes | 4 × vec4, cache-line aligned |
| Max particles/emitter | 2048 | ~128 KB per SSBO |
| Max concurrent emitters | 32 | Total ~4 MB SSBOs |
| Particle atlas | Variable | Shared across all emitters |

### 11.3 Descriptor Pool Pooling

- **Single pool** with MAX_CONCURRENT_EMITTERS descriptor sets pre-allocated
- **Per-emitter allocation** (no fragmentation risk)
- **Free-on-individual-destroy** flag enabled (supports out-of-order cleanup)

### 11.4 Lock-Free Queue Benefits

- **Zero mutex overhead** (atomics only)
- **Cache-line padding** prevents false sharing
- **No allocation** (fixed 256-slot ring buffer)
- **Bounded latency** (no malloc/free in hot path)

---

## 12. TROUBLESHOOTING & COMMON ISSUES

### Issue: Particles not rendering
- Check: ParticleSystem::isAlive() returning false early?
- Check: Descriptor set binding 1 (atlas texture) is valid?
- Check: Pipeline has no depth write, depth test enabled?

### Issue: Fog not visible
- Check: fogStart/fogEnd in correct range?
- Check: fogDensity too high (completely opaque)?
- Check: fogColor matches sky (may blend into background)?

### Issue: VFX events dropped
- Check: Queue full? (256 max, check game frame rate)
- Check: Render thread slow, not draining queue?

### Issue: Particle simulation wrong
- Check: dt being passed to compute shader (currently hardcoded to 1/60)?
- Check: Gravity direction (should be -Y for downward)?

---

## 13. SUMMARY TABLE: FILES & PURPOSES

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

