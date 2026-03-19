# Glory Engine — Implementation Plan

> **Sources:** PLAN, GLORY_ENGINE_AAA_IMPLEMENTATION_PLAN, GLORY_ENGINE_DEEP_DIVE_ISSUES_Version1, OPTIMIZATION_PLAN, MODEL_RENDERING_FIXES, CHARACTER_MOVEMENT_AND_ANIMATION_DEEP_DIVE, CUSTOM_ANIMATION_SYSTEM_IMPROVEMENTS, Vulkan_VFX_TDD_for_MOBA, GLB_INVESTIGATION_INDEX, GLB_PIPELINE_ANALYSIS

---

## Status Legend

✅ Done | 🔧 In Progress | ⬜ Pending | 🔴 Blocking

---

## 1. Critical Fixes (P0 — must ship)

### 1.1 Completed Core Engine Phases

All 15 foundational renderer phases are ✅ Done:

| Phase | Status | Notes |
|-------|--------|-------|
| 1. Core Loop | ✅ Done | GLFW window, Vulkan 1.3 instance, GPU scoring, swapchain, sync |
| 2. Triangle | ✅ Done | Merged into Phase 1 (Pipeline, RenderPass, Framebuffers) |
| 3. Buffers | ✅ Done | VMA, staging uploads, vertex/index buffers |
| 4. Camera | ✅ Done | FPS camera, UBOs, input manager |
| 5. Model Loading | ✅ Done | OBJ loader, depth testing, cube factory |
| 6. Textures | ✅ Done | stb_image, VkSampler, combined image sampler |
| 7. Lighting | ✅ Done | Blinn-Phong with LightUBO, normal matrix |
| 8. ECS | ✅ Done | EnTT registry, entity-driven render loop |
| 9. Materials | ✅ Done | Per-material descriptor sets, multi-object rendering |
| 10. Deferred | ✅ Done | G-buffer MRT infrastructure (forward path active) |
| 11. Shadows | ✅ Done | 2048×2048 shadow map wired into render loop, PCF filtering |
| 12. Post-Process | ✅ Done | HDR target → ACES tone mapping → swapchain |
| 13. Audio/Physics | ✅ Done | Stub interfaces + ECS components |
| 14. Editor | ✅ Done | Dear ImGui overlay: FPS, frame time, camera, exposure/gamma controls, F1 toggle |
| 15. Optimization | ✅ Done | Profiler with scoped timers, view-frustum culling (Gribb–Hartmann) |

### 1.2 🔴 BLOCKING: Vulkan Queue Family Ownership Bug (Textures Blank on MoltenVK)

**Symptom:** All textures render as blank/black on Apple Silicon (Mac M4) via MoltenVK.

**Root cause:** The engine selects a dedicated DMA transfer queue when available and uploads all texture/buffer data through it, but never transfers ownership of those resources to the graphics queue family. On NVIDIA/AMD desktop drivers this works by accident; on MoltenVK (which is strict about Vulkan spec) it causes undefined resource visibility.

#### Fix A — `src/renderer/Image.cpp` (PRIMARY FIX)

Replace image creation with concurrent sharing when dedicated transfer exists:

```cpp
VkImageCreateInfo imgCI{};
imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
imgCI.imageType     = VK_IMAGE_TYPE_2D;
imgCI.extent        = { width, height, 1 };
imgCI.mipLevels     = 1;
imgCI.arrayLayers   = 1;
imgCI.format        = format;
imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
imgCI.usage         = usage;
imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;

auto families = device.getQueueFamilies();
uint32_t queueFamilyIndices[] = {
    families.graphicsFamily.value(),
    families.transferFamily.value()
};

if (device.hasDedicatedTransfer()) {
    imgCI.sharingMode           = VK_SHARING_MODE_CONCURRENT;
    imgCI.queueFamilyIndexCount = 2;
    imgCI.pQueueFamilyIndices   = queueFamilyIndices;
} else {
    imgCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
}
```

**Note:** `TextureStreamer.cpp` already does this correctly — this is just applying the same pattern to `Image.cpp`.

#### Fix B — `src/renderer/Buffer.cpp` (createDeviceLocal)

```cpp
VkBufferCreateInfo bufCI{};
bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
bufCI.size  = size;
bufCI.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

auto families = device.getQueueFamilies();
uint32_t familyIndices[] = {
    families.graphicsFamily.value(),
    families.transferFamily.value()
};
if (device.hasDedicatedTransfer()) {
    bufCI.sharingMode           = VK_SHARING_MODE_CONCURRENT;
    bufCI.queueFamilyIndexCount = 2;
    bufCI.pQueueFamilyIndices   = familyIndices;
} else {
    bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
}
```

#### Fix C — `src/renderer/Texture.cpp` — Pipeline barrier stage mismatch

The second barrier (`TRANSFER_DST → SHADER_READ_ONLY`) is submitted to the transfer queue but uses `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` — invalid on transfer queues:

```cpp
} else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
           newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; // Fix: was FRAGMENT_SHADER_BIT
}
```

#### Fix D — `src/renderer/Texture.cpp` — VMA flush

After `staging.unmap()`:

```cpp
vmaFlushAllocation(device.getAllocator(), staging.getAllocation(), 0, VK_WHOLE_SIZE);
```

**Platform behavior:**

| Platform | Queue Families | Bug Impact |
|----------|---------------|-----------|
| NVIDIA (desktop) | Family 0: GFX+Compute+Transfer, Family 1: Transfer-only | Bug exists but driver is lenient |
| AMD (desktop) | Family 0: GFX+Compute+Transfer, Family 2: Transfer-only | AMD driver hides bug |
| Apple Silicon (MoltenVK M4) | May expose separate transfer family | **Bug triggers → textures blank** |

**Files to modify:** `src/renderer/Image.cpp`, `src/renderer/Buffer.cpp`, `src/renderer/Texture.cpp`

---

### 1.3 GLB Texture Pipeline Bugs

#### Bug 1 🔴 (Critical): Map Tiles Using Default Texture Instead of GLB Textures

**Root cause:** In `buildScene()`, the "Flat LoL-style map" section creates map entities using `makeTile()` lambdas that always assign `defaultTex`. When the GLB map model is loaded, its textures are extracted via `Model::loadGLBTextures()` but no code creates a `MapComponent` entity using the GLB mesh + its extracted textures.

**Fix (`Renderer.cpp` `buildScene()`):**

```cpp
std::string mapGlbPath = std::string(MODEL_DIR) + "fantasy+arena+3d+model.glb";
try {
    auto bounds = Model::getGLBBounds(mapGlbPath);
    float width  = bounds.max.x - bounds.min.x;
    float depth  = bounds.max.z - bounds.min.z;
    float uniScale = 200.0f / std::max(width, depth);

    uint32_t mapMeshIdx = m_scene.addMesh(
        Model::loadFromGLB(*m_device, m_device->getAllocator(), mapGlbPath));

    auto mapTextures = Model::loadGLBTextures(*m_device, mapGlbPath);
    uint32_t mapTexIdx = defaultTex;
    if (!mapTextures.empty()) {
        mapTexIdx = m_scene.addTexture(std::move(mapTextures[0]));
    }
    for (size_t i = 1; i < mapTextures.size(); ++i) {
        m_scene.addTexture(std::move(mapTextures[i]));
    }

    uint32_t texCount = static_cast<uint32_t>(m_scene.getTextures().size());
    for (uint32_t t = 0; t < texCount; ++t) {
        auto &tex = m_scene.getTexture(t);
        m_descriptors->writeBindlessTexture(t, tex.getImageView(), tex.getSampler());
    }

    auto mapEntity = m_scene.createEntity("GLBMap");
    m_scene.getRegistry().emplace<MeshComponent>(mapEntity, MeshComponent{mapMeshIdx});
    m_scene.getRegistry().emplace<MaterialComponent>(
        mapEntity, MaterialComponent{mapTexIdx, flatNorm, 0.0f, 0.0f, 1.0f});
    m_scene.getRegistry().emplace<ColorComponent>(
        mapEntity, ColorComponent{glm::vec4(1.0f)});  // white = show texture as-is
    m_scene.getRegistry().emplace<MapComponent>(mapEntity);

    auto &mapT = m_scene.getRegistry().get<TransformComponent>(mapEntity);
    mapT.position = glm::vec3(
        100.0f - ((bounds.min.x + width / 2.0f) * uniScale),
        -(bounds.min.y * uniScale),
        100.0f - ((bounds.min.z + depth / 2.0f) * uniScale));
    mapT.scale = glm::vec3(uniScale);

    m_glbMapLoaded = true;
} catch (const std::exception &e) {
    spdlog::warn("Failed to load GLB map, falling back to flat tiles: {}", e.what());
    m_glbMapLoaded = false;
}

if (!m_glbMapLoaded) {
    m_customMap = true;
    // ... existing makeTile/makeStrip code ...
}
```

#### Bug 2 🔴 (Critical): Axis Mismatch — Models Appear Sideways

**Root cause:** GLB files use Y-up right-handed coordinates, but many tools (Blender default) export Z-up. The engine applies no axis correction.

**Fix:** Add Z-up heuristic detection:

```cpp
float height  = bounds.max.y - bounds.min.y;
float zExtent = bounds.max.z - bounds.min.z;
bool isZUp = (height > zExtent * 2.0f); // Y range >> Z range → Z-up model

if (isZUp) {
    mapT.rotation.x = glm::radians(-90.0f);  // -90° X: Z-up → Y-up
    width  = bounds.max.x - bounds.min.x;
    depth  = bounds.max.y - bounds.min.y;  // was height in Z-up
    uniScale = 200.0f / std::max(width, depth);
    mapT.position = glm::vec3(
        100.0f - ((bounds.min.x + width / 2.0f) * uniScale),
        -(bounds.min.z * uniScale),
        100.0f - ((bounds.min.y + depth / 2.0f) * uniScale));
    mapT.scale = glm::vec3(uniScale);
    spdlog::info("GLB map detected as Z-up, applied -90° X rotation");
}
```

Character/skinned models may also need: `charT.rotation.x = glm::radians(-90.0f);`

#### Bug 3 (High): Vertex Color Tint Washing Out GLB Textures

**Fix:** Set `ColorComponent` to pure white for GLB entities:

```cpp
m_scene.getRegistry().emplace<ColorComponent>(
    mapEntity, ColorComponent{glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
```

#### Bug 4 (Medium): Multi-Material GLB Models Show Only One Texture

**Root cause:** `MaterialComponent` holds a single `materialIndex`. GLB models with multiple primitives each referencing different materials only show the first texture.

**Fix (Approach A — per-primitive material tracking):**

In `GLBLoader.cpp`, inside the primitive loop:
```cpp
int primMatIdx = prim.material;
model.m_meshMaterialIndices.push_back(primMatIdx);
```

In `Model.h`:
```cpp
std::vector<int> m_meshMaterialIndices;
int getMeshMaterialIndex(uint32_t meshIdx) const {
    return (meshIdx < m_meshMaterialIndices.size())
        ? m_meshMaterialIndices[meshIdx] : -1;
}
```

**Fix (Approach B — per-submesh entity creation):**

Create one entity per GLB primitive, each with its own `MeshComponent` + `MaterialComponent`.

#### Bug 5 (Medium): loadGLBTextures() Texture-to-Mesh Mapping Lost

**Fix:** Return material index alongside texture:

```cpp
struct GLBTextureInfo {
    Texture texture;
    int     materialIndex;  // which glTF material this came from
};

// Build side-channel in loadGLBTextures:
std::unordered_map<int, uint32_t> materialToTexIdx;

// Caller uses:
// model.getMeshMaterialIndex(mi) → materialToTexIdx → scene texture index
```

#### Map Asset Filename Issues

```
read_team_tower_2.glb  ⚠️  TYPO: should be "red_team_tower_2.glb"
red_team_tower3.glb    ⚠️  MISSING UNDERSCORE: should be "red_team_tower_3.glb"
```

These filenames will fail silently on case-sensitive filesystems. Fix by renaming files and updating `buildScene()` references.

### 1.4 GLB Loading Pipeline Summary

**Function call chain:**
```
Renderer::buildScene()
├─ Model::loadFromGLB(device, allocator, path)         → mesh vertex data
│   └─ tinygltf: POSITION, NORMAL, TEXCOORD_0, INDICES
│   └─ Returns Model{m_meshes[], m_meshMaterialIndices[]}
├─ Scene::addMesh(model)                                → uint32_t meshIdx
├─ Model::loadGLBTextures(device, path)                 → std::vector<GLBTexture>
│   └─ tinygltf: materials[].pbrMetallicRoughness.baseColor only (no PBR maps yet)
│   └─ RGB→RGBA expansion, stb_image decode
│   └─ ⚠️ Only glbTextures[0] used currently!
├─ Scene::addTexture(texture)                           → uint32_t texIdx
├─ BindlessDescriptors::registerTexture()               → bindless array slot
├─ Scene::createEntity()
│   ├─ MeshComponent{meshIdx}
│   └─ MaterialComponent{texIdx, normalIdx, shininess, metallic, roughness, emissive}
└─ [Render pass] GpuObjectData.texIndices = {texIdx, normalIdx, 0, 0}
   └─ Fragment shader: texture(textures[nonuniformEXT(fragDiffuseIdx)], uv)
```

**Key structs:**

```
MaterialComponent:  24 bytes  (materialIndex, normalMapIndex, shininess, metallic, roughness, emissive)
GpuObjectData:     224 bytes  (model mat4, normalMatrix mat4, aabbMin/Max, tint, params, texIndices, mesh offsets)
Vertex (GPU):       44 bytes  (position, color, normal, texCoord)
```

**Shader binding locations:**

| Binding | Set | Purpose |
|---------|-----|---------|
| 0 | 0 | Camera/transform UBO |
| 2 | 0 | Light UBO (4 lights, fog params) |
| 3 | 0 | Shadow map |
| 5 | 0 | Toon ramp (256×1) |
| 6 | 0 | Fog of War (512×512 R8) |
| 7 | 0 | Scene SSBO (GpuObjectData[]) |
| 0 | 1 | Bindless textures (sampler2D[4096]) |

### 1.5 Priority Order for Critical Fixes

1. **Fix 1.2** (🔴 Blocking) — Queue family ownership bug → blank textures on MoltenVK
2. **Bug 1** (🔴 Critical) — GLB map not using its embedded textures
3. **Bug 2** (🔴 Critical) — Axis mismatch → models sideways
4. **Bug 3** (High) — Color tint washing out textures (easy 1-liner)
5. **Bug 4** (Medium) — Multi-material GLB support
6. **Bug 5** (Medium) — Texture mapping accuracy (needed for Bug 4)

---

## 2. Core Gameplay (P1)

### 2.1 Implementation Status

| System | Status | Notes |
|--------|--------|-------|
| Core Loop (30Hz fixed-step) | ✅ Done | `FIXED_DT = 1/30 s` |
| Vulkan Backend | ✅ Done | Device, swapchain, frames-in-flight |
| Shadow Mapping | ✅ Done | PCF, 2048×2048 |
| Hi-Z Occlusion Culling | ✅ Done | Gribb–Hartmann 6-plane frustum |
| Bloom, SSAO, SSR | ✅ Done | HDR pipeline |
| Navigation | ✅ Done | Recast/Detour + flow fields |
| Ability & Combat | ✅ Done | Multi-stage ability execution, melee combat |
| Networking | ✅ Done | Low-latency input sync, state snapshots |

### 2.2 Mandatory Prerequisite: Renderer Decomposition

**Problem:** `src/renderer/Renderer.cpp` is 170,473 bytes / 3,758 lines. It contains the game loop, scene construction, simulation stepping, minion management, bone slot pooling, and Vulkan command recording. No agent can reliably modify it without decomposition.

**Action:** Extract fixed-timestep simulation into `src/core/SimulationLoop.h/.cpp`.

```cpp
// src/core/SimulationLoop.h
struct SimulationContext {
    Scene*             scene;
    InputManager*      input;
    ProjectileSystem*  projectileSystem;
    MinionSystem*      minionSystem;
    StructureSystem*   structureSystem;
    JungleSystem*      jungleSystem;
    AutoAttackSystem*  autoAttackSystem;
    ParticleSystem*    particles;
    HeightQueryFn      heightFn;
    float*             gameTime;
    bool               mobaMode;
    bool               customMap;
};

class SimulationLoop {
public:
    static constexpr float FIXED_DT  = 1.0f / 30.0f;
    static constexpr float MAX_DELTA = 0.25f;
    static constexpr int   MAX_STEPS = 8;

    void tick(SimulationContext& ctx, float deltaTime);

    float getAccumulator() const { return m_accumulator; }
    float getAlpha()       const { return m_alpha; }
private:
    float m_accumulator = 0.0f;
    float m_alpha       = 0.0f;
};
```

Simulation update order (extracted from `Renderer.cpp`):
```
Tick N (FIXED_DT = 1/30 s):
  1.  m_input->update(FIXED_DT)
  2.  m_scene.update(FIXED_DT, m_currentFrame)      [character movement + animation]
  3.  m_projectileSystem.update(m_scene, FIXED_DT)
  4.  [if mobaMode && !customMap]
      a. m_minionSystem.update(registry, FIXED_DT, m_gameTime, heightFn)
      b. m_structureSystem.update(registry, FIXED_DT, m_gameTime)
      c. m_jungleSystem.update(registry, FIXED_DT, m_gameTime, heightFn)
      d. m_autoAttackSystem.update(registry, m_minionSystem, FIXED_DT)
      e. Process structure/jungle death events
  5.  Bone slot cleanup for destroyed minion entities
  6.  Assign render components to newly spawned minion entities
  7.  m_particles->update(FIXED_DT)
```

### 2.3 Fixed-Point Deterministic Math (Phase 0.1)

Replace `float` simulation math with `Fixed64` (16-bit fractional, Q48.16):

```cpp
class Fixed64 {
    static constexpr int     FRAC_BITS = 16;
    static constexpr int64_t ONE       = int64_t(1) << FRAC_BITS;
    int64_t raw = 0;

    static constexpr Fixed64 fromInt(int32_t v)   { return Fixed64(int64_t(v) << FRAC_BITS); }
    static constexpr Fixed64 fromFloat(float f)   { return Fixed64(int64_t(f * ONE)); }
    float toFloat() const { return float(raw) / float(ONE); }

    Fixed64 operator*(Fixed64 o) const {
        __int128 temp = static_cast<__int128>(raw) * o.raw;  // GCC/Clang
        return Fixed64(static_cast<int64_t>(temp >> FRAC_BITS));
    }
};
```

Use `Fixed64` only in Sim* (simulation) components; render components remain `float`. Requires `GLM_FORCE_DEPTH_ZERO_TO_ONE` for Vulkan.

### 2.4 State Snapshots and Checksums (Phase 0.3)

Per-tick checksum for deterministic replay/netcode validation:
```cpp
// StateChecksumSystem: runs at end of every tick
uint32_t hash = MurmurHash3_x86_32(
    simComponents.data(), simComponents.size() * sizeof(SimComponent), 0);
snapshots[tickIndex] = { hash, copyAllSimComponents() };
```

### 2.5 Core Gameplay Features

| Feature | Status | Files |
|---------|--------|-------|
| Minion system (wave spawn) | 🔧 In Progress | `src/minion/MinionSystem.h/.cpp` |
| Tower/Inhibitor/Nexus | 🔧 In Progress | `src/structure/StructureSystem.h/.cpp` |
| Jungle monster AI | 🔧 In Progress | `src/jungle/JungleSystem.h/.cpp` |
| Auto-attack system | ⬜ Pending | `src/combat/AutoAttackSystem.h` |
| Economy (gold/XP) | ⬜ Pending | in DeathSystem |
| Ability system full kit | ⬜ Pending | `src/ability/` |
| HUD overlay | ⬜ Pending | `src/hud/HUD.h/.cpp` |
| Click indicator | ⬜ Pending | `src/renderer/ClickIndicatorRenderer.h/.cpp` |
| Fog of War | 🔧 In Progress | `src/fog/FogSystem.h/.cpp` (exists) |

Build gate after each P1 feature:
```sh
cmake --build build/ --parallel
cd build/ && ctest --output-on-failure
```

---

## 3. Animation & Movement (P2)

### 3.1 Animation System Status

| Feature | Status | Notes |
|---------|--------|-------|
| CPU skeletal animation | ✅ Done | `CPUSkinning.cpp`, `AnimationPlayer` |
| GPU vertex shader skinning | ✅ Done | `skinned.vert`, bone SSBO binding 4 |
| GPU compute skinning (>50 entities) | ✅ Done | `ComputeSkinner`, `skinning.comp` |
| SkinnedLOD (3 levels) | ✅ Done | 10K/3K/700 tris at 20m/60m/∞ |
| Animation blending/crossfade | ✅ Done | `AnimationPlayer::crossfadeTo()` |
| Root motion extraction | ⬜ Pending | — |
| IK solver | ⬜ Pending | — |
| Blend trees | ⬜ Pending | — |

### 3.2 CPU Skinning Performance Issue

**Problem:** When a minion wave spawns (24 minions), the app drops to **0.2 FPS** (CPU Simulation: 4,507 ms/frame).

- Melee minion: 26,760 vertices, 65 joints
- Caster minion: 29,663 vertices, 41 joints
- 24 minions × ~28K verts = ~670K vertices CPU-skinned + re-uploaded to GPU every frame
- Each `DynamicMesh::updateVertices` does `memcpy(44 bytes × vertexCount)` → ~28 MB/frame

**Fix A: Mesh Decimation (immediate, 2 lines)**

```cpp
// In Renderer.cpp buildScene(), minion template loading:
// OLD: loader.load("models/melee_minion/melee_minion_walking.glb", 0.0f);
// NEW:
loader.load("models/melee_minion/melee_minion_walking.glb", 0.6f);
```

- 27K → ~10K verts per minion (60% reduction)
- `targetIndexCount = max(indices * 0.4, 900)`, `targetError = 0.02f`, min 300 tris

**Fix B: GPU Skinning for Minions (~200 lines)**

Switch minions from `DynamicMeshComponent` (CPU) to `GPUSkinnedMeshComponent` (GPU compute):

| File | Change |
|------|--------|
| `Descriptors.h:87` | `MAX_SKINNED_CHARS` 32 → **128** |
| `Renderer.h` | Add `m_freeBoneSlots` queue + `m_entityBoneSlot` map |
| `Renderer.cpp` (buildScene) | Pre-fill slots 0..127; create shared `StaticSkinnedMesh` per minion type |
| `Renderer.cpp` (minion spawn) | Replace `DynamicMeshComponent` → `GPUSkinnedMeshComponent`; allocate bone slot |
| `Renderer.cpp` (minion death) | Return bone slot; destroy output buffer; remove from `m_computeSkinEntries` |
| `Scene.cpp` | `writeBoneSlot(frameIdx, slot, matrices)` for GPU-skinned minions |
| `MinionSystem.cpp` | On death callback, release bone slot |

**Expected result:** CPU skinning 4,507 ms → **0 ms**. GPU compute: 24 × ~0.05 ms = **~1.2 ms**. Target: 60+ FPS.

**Key constants:**

| File | Constant | Current | Target | Purpose |
|------|----------|---------|--------|---------|
| `Descriptors.h:87` | `MAX_SKINNED_CHARS` | 32 | 128 | Concurrent skinned entities |
| `ComputeSkinner.h:24` | `COMPUTE_SKIN_THRESHOLD` | 50 | 1 | Always use compute path |
| GLBLoader calls | `targetReduction` | 0.0f | 0.6f | Minion mesh decimation |

### 3.3 Animation State Machine

```
Idle ──(hasTarget && dist > 0.1)──► Walk
Walk ──(!hasTarget || dist ≤ 0.1)──► Idle
Walk/Idle ──(ability cast)──────────► Cast
Cast ──(castTime elapsed)───────────► Walk or Idle
Any ──(death)───────────────────────► Die
```

Quaternion Slerp for smooth rotation:
```cpp
float totalAngle = std::acos(Quaternion::Dot(currentRot, targetRot));
if (totalAngle > 0.001f) {
    float t = (turnSpeed * deltaTime) / totalAngle;
    t = std::clamp(t, 0.0f, 1.0f);
    currentRot = Slerp(currentRot, targetRot, t);
} else {
    currentRot = targetRot;  // Snap to avoid micro-jitter
}
```

### 3.4 Custom Animation System Improvements

**Planned improvements (⬜ Pending):**

1. **Blend Trees** — weighted multi-clip blending (e.g., 30% idle + 70% walk during acceleration)
2. **Root Motion** — extract root bone delta per frame → character movement driven by animation
3. **IK Solver** — 2-bone analytical IK for foot placement on uneven terrain
4. **Animation Events** — keyframe callbacks for footstep sounds, VFX spawn, hitbox activation
5. **Retargeting** — bone space remapping to share animations across differently-rigged characters

**Animation clip indexing convention:**
```
Clip[0] = Idle
Clip[1] = Walk
Clip[2] = Attack (auto-attack swing)
Clip[3] = Ability_Q
Clip[4] = Ability_W
Clip[5] = Ability_E
Clip[6] = Ability_R
Clip[7] = Death
Clip[8] = Recall
```

### 3.5 Character Movement

```cpp
struct CharacterComponent {
    glm::vec3 targetPosition{0.0f};       // Right-click destination
    float     moveSpeed = 6.0f;           // Units per second
    bool      hasTarget = false;
    glm::quat currentFacing{1.0f,0,0,0}; // Smooth rotation state
    float     currentSpeed = 0.0f;        // Acceleration towards moveSpeed
};
```

Movement update flow:
1. **Right-click** → `IsometricCamera::screenToWorldRay()` → ray-plane Y=0 intersection → `character.targetPosition`
2. **Scene::update()** → move character toward target at `moveSpeed * dt`
3. **Terrain snap** → `TerrainSystem::GetHeightAt(x, z)` sets Y position
4. **Smooth rotation** → Quaternion Slerp toward movement direction
5. **Stop** when distance < 0.1 units

---

## 4. Performance & Optimization (P2)

### 4.1 Performance Status

**Completed optimizations:**

| Phase | Optimization | Result |
|-------|-------------|--------|
| Phase 1 | Mesh decimation (meshoptimizer) | 492K → ~10K verts (97% reduction) |
| Phase 1 | Skip 200+ demo entities in MOBA mode | Major CPU + GPU save |
| Phase 1 | Light-frustum culling on shadow pass | Fewer shadow draw calls |
| Phase 1 | SSAO + Bloom skipped in MOBA mode | Save ~60MB GPU memory + 3 passes |
| Phase 1 | FXAA + ACES tone map only in MOBA | Eliminate post-process overhead |
| Phase 2 | GPU vertex shader skinning | `skinned.vert`, bone SSBO |
| Phase 2 | Static device-local GPU mesh | Uploaded once, never per-frame |
| Phase 2 | Ring-buffer bone SSBO (32 slots) | Per-entity bone slot management |
| Phase 2 | SkinnedLOD (3 levels) | 10K/3K/700 tris |
| Phase 2 | Fixed-timestep 30Hz simulation | CPU decoupled from render |
| Phase 2 | GPU timestamp profiler | Per-pass GPU timings in overlay |
| Phase 2 | Post-process specialization constants | MOBA pipeline vs full pipeline |
| Phase 2 | Particle distance LOD | Emission scales 0–100% by camera distance |
| Phase 2 | Shadow frustum tightened to view | No wasted shadow map texels |
| Phase 3 | GPU frustum culling compute shader | `cull.comp` + `GpuCuller` |
| Phase 3 | Cascaded Shadow Maps (CSM) | 3 cascades, 2048×2048 depth array |
| Phase 3 | Dedicated transfer queue | DMA offload for uploads |
| Phase 3 | Terrain CDLOD | LOD0/1/2 at 40m/80m/∞, ~75% index reduction |
| Phase 3 | Async texture streaming | Background upload, zero GFX-queue stalls |
| Phase 3 | Compute skinning | Pre-skin in compute pass (>50 entity threshold) |
| Phase 3 | GPU-driven draw merging | `GpuCuller` with instance output SSBO |

### 4.2 Performance Budget (1080p, 60 FPS)

| System | Budget | Current Cost |
|--------|--------|-------------|
| Shadow pass | 1.5 ms | ~0.5 ms (1 char + terrain) |
| Scene geometry | 3.0 ms | ~1.0 ms (MOBA mode) |
| Post-process | 0.5 ms | ~0.3 ms (tone map + FXAA) |
| CPU simulation (30 Hz) | 2.0 ms | ~0.3 ms |
| CPU draw recording | 1.0 ms | ~0.5 ms |
| **Total frame budget** | **16.6 ms** | **~3 ms (MOBA mode)** |

### 4.3 Triangle Budget (Full MOBA Match)

| Asset | Triangles | Count | Total |
|-------|-----------|-------|-------|
| Player Champion | 10,000 | 1 | 10,000 |
| Other Champions | 5,000 | 9 | 45,000 |
| Minions | 500 | 60 | 30,000 |
| Towers | 3,000 | 11 | 33,000 |
| Terrain | 65,000 | 1 | 65,000 |
| Trees/Rocks/Deco | 200 | 500 | 100,000 |
| Particles/FX | 50 | 200 | 10,000 |
| **TOTAL** | | | **~293K** |

Target: stay under 400K triangles for 60 FPS on integrated GPU.

### 4.4 Remaining Optimization Work

**Phase 3 (remaining, ⬜ Pending):**

- [ ] **Hi-Z Occlusion Culling** — Hierarchical Z-Buffer in `GpuCuller` to skip objects hidden behind terrain/structures
- [ ] **Multi-Threaded Command Recording** — Vulkan secondary command buffers across all CPU cores (framework exists: `m_workerCount`, per-thread command pools)
- [ ] **Bindless Resources** — More aggressive `VK_EXT_descriptor_indexing` adoption to reduce descriptor set management overhead

**Phase 4 (future, ⬜ Pending):**

- [ ] **Cooked Asset Pipeline** — Convert GLB → custom binary format at build time; eliminate `tinygltf` runtime cost
- [ ] **Async Compute Pipeline** — Overlap particle simulation with geometry rendering on separate compute queue
- [ ] **Visibility Buffer Rendering** — GPU-side geometry cache for draw call merging
- [ ] **Pipeline Libraries** — `VK_EXT_graphics_pipeline_library` to reduce shader compilation stalls

### 4.5 SSAO + Bloom Configuration

In MOBA mode both are disabled at startup:
```cpp
if (!m_mobaMode) {
    m_bloom = std::make_unique<BloomPass>(...);
    m_ssao  = std::make_unique<SSAO>(...);
} else {
    // 1×1 dummy images bound to descriptors
}
```

To enable bloom in MOBA mode: flip `m_mobaMode = false` or add an explicit `m_bloomEnabled` toggle.

---

## 5. VFX & Polish (P3)

### 5.1 VFX TDD Plan

Unit test coverage targets for the VFX system:

| Test | Status | Description |
|------|--------|-------------|
| `test_vfx_spawn_basic` | ⬜ Pending | Push `VFXEvent::Spawn`, verify `ParticleSystem` created |
| `test_vfx_queue_full` | ⬜ Pending | Push 257 events into 256-slot ring; verify oldest dropped |
| `test_vfx_lifecycle` | ⬜ Pending | Spawn effect, advance dt past duration, verify `isAlive()=false` |
| `test_vfx_destroy_early` | ⬜ Pending | Push Spawn then Destroy, verify immediate teardown |
| `test_vfx_move_event` | ⬜ Pending | Spawn, Move, verify emitter position updated |
| `test_emitter_def_load` | ⬜ Pending | Load JSON EmitterDef, verify all fields parsed |
| `test_particle_gravity` | ⬜ Pending | Advance sim 1s, verify particle Y < initial Y by gravity × t² |
| `test_particle_death` | ⬜ Pending | Set lifetime=0, verify `params.w < 0.5` after sim step |
| `test_descriptor_binding` | ⬜ Pending | Verify descriptor set layout (binding 0=SSBO, 1=sampler) |
| `test_billboard_expand` | ⬜ Pending | Verify vertex shader expands SSBO particle → 6 screen vertices |

### 5.2 VFX Integration Tests

| Test | Status | Description |
|------|--------|-------------|
| `test_ability_fires_vfx` | ⬜ Pending | Cast fireball ability, verify `vfx_fireball_cast` spawned in queue |
| `test_projectile_vfx_track` | ⬜ Pending | Projectile moves, verify VFX Move events each frame |
| `test_impact_vfx_triggered` | ⬜ Pending | Projectile hits target, verify explosion VFX spawned at impact pos |
| `test_vfx_no_leak` | ⬜ Pending | Spawn 100 effects, wait for expiry, verify 0 active effects |
| `test_vfx_thread_safety` | ⬜ Pending | Push events from game thread while render thread drains; no data race |

### 5.3 VFX Polish Items (⬜ Pending)

- [ ] **Atlas-based sprite animation** — support `atlasFrame` field in `GpuParticle.params.z`
- [ ] **Trail ribbons** — vertex-ribbon mesh spawned along projectile path
- [ ] **Soft particles** — depth-test fade at intersection with geometry
- [ ] **Distortion** — `DistortionRenderer` post-process pass (refraction)
- [ ] **Shield bubble** — `ShieldBubbleRenderer` — transparent fresnel mesh
- [ ] **Cone ability mesh** — `ConeAbilityRenderer` — W-ability AoE indicator
- [ ] **Explosion** — `ExplosionRenderer` — E-ability multi-burst radial particles
- [ ] **Sprite VFX** — `SpriteEffectRenderer` — atlas-based sprite sheets

### 5.4 Fog of War (Phase 3)

**Status:** `src/fog/FogSystem.h/.cpp` + `shaders/fog.vert/frag` already exist. Do NOT recreate.

FOW texture: 512×512 R8, bound at set=0, binding=6. Fragment shader samples it via world XZ → UV mapping.

Remaining:
- [ ] Compute shader update for vision radius queries
- [ ] Integration with minimap rendering
- [ ] Team-aware visibility (Blue/Red teams have separate FOW masks)

---

## 6. AAA Feature Roadmap (P4 — future)

### 6.1 Networking — Lock-Step with Rollback (Phase 1.1)

**Design:**
- Deterministic lockstep at 30 Hz with input latency compensation
- Rollback netcode: store up to 8 frames of state snapshots
- Client predicts locally, server authoritative for ability resolution

**Required infrastructure:**
- `Fixed64` simulation math (prerequisite)
- Per-tick state checksums (prerequisite)
- `ReplaySystem.h/.cpp` (already stub-implemented)
- ENet transport (already integrated)

```cpp
// Net messages
struct InputFrame { uint32_t tick; uint8_t buttons; int16_t mouseX, mouseY; };
struct ConfirmedInputFrame { uint32_t tick; std::array<InputFrame, 10> playerInputs; };
struct StateChecksum { uint32_t tick; uint32_t hash; };
```

### 6.2 Flow Fields and Steering (Phase 2.1)

- Replace waypoint-following with flow field for minion navigation
- Per-lane flow field (128×128 grid, 1 field per team per lane)
- Agent steering: `velocity += (flowDir * flowWeight) + (sep * sepWeight) + (arrive * arriveWeight)`
- Rebuild flow field only on lane change events, not per-frame

### 6.3 Hi-Z Occlusion Culling (Phase 4.2)

```
1. Depth pre-pass (render all opaque at full res)
2. Downsample depth to HiZ pyramid (mip chain, 1024→1→2×2→...→1×1)
3. Compute shader reads HiZ pyramid
4. For each entity AABB: project 8 corners, find depth range
5. Compare min depth against HiZ sample → cull if behind
6. Only survivors get indirect draw commands
```

### 6.4 Bindless Resources Expansion (Phase 4.3)

Current status: partially implemented (4096-sampler array at set=1, binding=0).

Remaining:
- Bindless vertex/index buffers (reduce VkBuffer descriptor count)
- Bindless per-frame data UBOs
- Requires `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` with `runtimeDescriptorArray`

### 6.5 Cooked Asset Pipeline (Phase 6)

```
Build time:
  Raw GLB → meshoptimizer simplification + vertex cache optimization
           → custom binary format (.glory_mesh):
              {header, vertices[], indices[], bones[], materialIndices[]}

Runtime:
  mmap() binary file → direct DMA upload (no tinygltf, no stb_image at runtime)

Startup time: 60% faster
Memory: 30% lower (no JSON overhead)
```

### 6.6 Advanced Debug / Editor Tools

- [ ] **Scene inspector:** Entity list, component editors via ImGui
- [ ] **Nav mesh visualization:** Overlay walkable areas + lane paths
- [ ] **Ability timeline:** Per-champion cooldown/buff timeline view
- [ ] **Replay playback:** Seek to any tick in recorded game
- [ ] **GPU capture integration:** RenderDoc / Metal Frame Capture hooks via VK_EXT_debug_utils

### 6.7 Post-MVP Features (P5)

| Feature | Notes |
|---------|-------|
| Dragon / Baron Nashor | Unique monster AI + global buffs |
| Objectives system | Turret plate gold, dragon soul stacks |
| Item shop UI | Gold economy → stat upgrades |
| 5v5 matchmaking | Server-authoritative with ENet |
| Spectator mode | Delayed state stream from server |
| Custom map editor | JSON/YAML lane placement tool |
| Water rendering | `water.vert/frag` shaders exist, need integration |
| Deferred lighting | G-Buffer path exists but forward path active |
| Motion blur | Velocity buffer per-pixel |
| Depth of Field | Circle of confusion from depth buffer |
| DLSS/FSR | Temporal upscaling for lower-spec GPUs |
