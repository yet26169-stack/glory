# MOBA Map Implementation Plan — Vulkan Custom Engine (C++)

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                      GAME LOOP                              │
│  Input → Update(dt) → Render → Present                     │
└────┬──────────┬───────────┬───────────┬───────────┬─────────┘
     │          │           │           │           │
 Phase 0    Phase 1     Phase 2     Phase 3     Phase 5
 Vulkan     MapData     Terrain     Shading     Fog of
 Bootstrap  + JSON      + Camera    + Water     War
                            │
                        Phase 4
                        Navigation
                        + Spawning
```

**Coordinate System (Global Convention)**
- Right-handed, Y-up
- X = Width (East/West), Z = Depth (North/South), Y = Height (Vertical)
- Map bounds: `(0, 0, 0)` to `(200, maxHeight, 200)`
- Map center: `(100, 0, 100)`
- Team 1 (Blue) base corner: near `(0, 0, 0)`
- Team 2 (Red) base corner: near `(200, 0, 200)`

**Core Dependencies**
- Vulkan 1.3+ (or 1.2 with extensions)
- GLM (mathematics)
- nlohmann/json (data loading)
- stb_image (heightmap loading)
- VMA — Vulkan Memory Allocator (buffer/image management)
- SPIR-V (shader compilation via glslangValidator or shaderc)

---

## Phase 0: Vulkan Bootstrap & Frame Infrastructure

### Goal
Stand up a minimal but properly structured Vulkan renderer that every subsequent phase builds on. Nothing renders visually yet — this phase produces a spinning empty frame with a clear color.

### 0.1 — Instance, Device & Surface

**Vulkan Instance Creation**
- Enable `VK_LAYER_KHRONOS_validation` in debug builds.
- Request `VK_KHR_surface` and the platform-specific surface extension (e.g., `VK_KHR_win32_surface`).
- Set up a `VkDebugUtilsMessengerEXT` callback that logs validation errors to `stderr` and optionally breaks into a debugger on errors.

**Physical Device Selection**
- Enumerate all `VkPhysicalDevice` handles.
- Score devices by: discrete GPU preferred, required queue family support (graphics + present + transfer), required extension support (`VK_KHR_swapchain`).
- Store chosen device properties (limits, memory types) in a global `GPUInfo` struct for later reference.

**Logical Device & Queues**
- Create a `VkDevice` with at least two queue families:
  - **Graphics/Present Queue** — used for rendering and presentation.
  - **Transfer Queue** (if available as a separate family) — used for async buffer/image uploads. If not separate, share the graphics queue.
- Store queue handles and family indices in a `VulkanContext` struct that is passed by reference throughout the engine.

### 0.2 — Swapchain

**Swapchain Creation**
- Query `VkSurfaceCapabilitiesKHR`, supported formats, and present modes.
- Prefer `VK_FORMAT_B8G8R8A8_SRGB` with `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`.
- Prefer `VK_PRESENT_MODE_MAILBOX_KHR` (triple buffer, low latency) with `VK_PRESENT_MODE_FIFO_KHR` as fallback (guaranteed available, vsync).
- Request `minImageCount + 1` images (typically 3).
- Store swapchain image views in a `std::vector<VkImageView>`.

**Swapchain Recreation**
- Implement a `RecreateSwapchain()` function triggered on `VK_ERROR_OUT_OF_DATE_KHR` or window resize events.
- This function must also recreate all framebuffers and depth images that depend on swapchain extent.

### 0.3 — Render Pass & Framebuffers

**Main Render Pass**
- Attachment 0: Color attachment (swapchain format), `loadOp = CLEAR`, `storeOp = STORE`, final layout `PRESENT_SRC_KHR`.
- Attachment 1: Depth/Stencil attachment (`VK_FORMAT_D32_SFLOAT` or `VK_FORMAT_D32_SFLOAT_S8_UINT`), `loadOp = CLEAR`, `storeOp = DONT_CARE`.
- One subpass consuming both attachments.
- Subpass dependency: external → subpass 0, stage `COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS`, access `COLOR_ATTACHMENT_WRITE | DEPTH_STENCIL_ATTACHMENT_WRITE`.

**Depth Image**
- Allocate via VMA with `VMA_MEMORY_USAGE_GPU_ONLY`.
- Create matching `VkImageView`.

**Framebuffers**
- One per swapchain image, each referencing the swapchain image view + the shared depth image view.

### 0.4 — Command Infrastructure

**Command Pool & Buffers**
- Create a `VkCommandPool` per frame-in-flight (not per swapchain image — per *in-flight frame*). Use `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`.
- Allocate one primary `VkCommandBuffer` per frame-in-flight.

**Frames in Flight**
- Use 2 frames in flight (standard for double-buffered command submission).
- Per frame, store:
  - `VkCommandBuffer`
  - `VkSemaphore imageAvailable`
  - `VkSemaphore renderFinished`
  - `VkFence inFlightFence`

### 0.5 — Frame Loop

```
while (!shouldClose) {
    pollEvents();

    // Wait for this frame's fence
    vkWaitForFences(fence[currentFrame]);
    vkResetFences(fence[currentFrame]);

    // Acquire next swapchain image
    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(..., imageAvailable[currentFrame], &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { RecreateSwapchain(); continue; }

    // Record command buffer
    vkResetCommandBuffer(cmd[currentFrame]);
    vkBeginCommandBuffer(cmd[currentFrame]);
      vkCmdBeginRenderPass(cmd[currentFrame], clearColor = {0.05, 0.05, 0.08, 1.0});
        // --- PHASE 2+ RENDERING GOES HERE ---
      vkCmdEndRenderPass(cmd[currentFrame]);
    vkEndCommandBuffer(cmd[currentFrame]);

    // Submit
    VkSubmitInfo submit = { waitSemaphore = imageAvailable, signalSemaphore = renderFinished };
    vkQueueSubmit(graphicsQueue, submit, fence[currentFrame]);

    // Present
    VkPresentInfoKHR present = { waitSemaphore = renderFinished, imageIndex };
    vkQueuePresentKHR(presentQueue, &present);

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}
vkDeviceWaitIdle(device);
```

### 0.6 — VMA Setup

- Initialize `VmaAllocator` immediately after device creation.
- Create a helper `BufferBuilder` utility:
  - `CreateBuffer(size, usage, memoryUsage)` → returns `VkBuffer` + `VmaAllocation`.
  - `CreateStagingBuffer(size, data)` → creates a host-visible buffer, maps it, copies data, returns handle.
  - `CopyBufferToBuffer(src, dst, size)` → records and submits a one-shot transfer command.

### 0.7 — Descriptor Set Layout (Forward Declaration)

Define a global UBO binding convention used by all subsequent shaders:

| Set | Binding | Name              | Type    | Contents                        |
|-----|---------|-------------------|---------|---------------------------------|
| 0   | 0       | `SceneUBO`        | Uniform | View matrix, Proj matrix, time, camera pos |
| 0   | 1       | `ModelUBO`        | Uniform | Model matrix, team color ID     |
| 1   | 0–N     | `Textures`        | Sampler | Bound per-material              |

This convention means every pipeline binds Set 0 for scene data, and Set 1 varies per material.

### Phase 0 Deliverables
- [ ] Window opens, Vulkan clears to a dark color, no validation errors.
- [ ] Swapchain recreates cleanly on window resize.
- [ ] VMA allocator works — test by allocating and freeing a dummy buffer.
- [ ] Frame timing: measure `dt` per frame, print FPS to console.

---

## Phase 1: Data-Driven Map System

### Goal
Define all map entity positions in a single JSON file for Team 1 + neutrals. The engine mirrors Team 1's data to produce Team 2 automatically, with optional manual overrides.

### 1.1 — C++ Data Structures

```cpp
// ─── MapTypes.h ───

#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <optional>
#include <array>

enum class TeamID : uint8_t { Blue = 0, Red = 1, Neutral = 2 };

enum class LaneType : uint8_t { Top = 0, Mid = 1, Bot = 2, Count = 3 };

enum class TowerTier : uint8_t { Outer = 0, Inner = 1, Inhibitor = 2, Nexus = 3 };

enum class CampType : uint8_t {
    RedBuff, BlueBuff, Wolves, Raptors, Gromp, Krugs,
    Scuttler, Dragon, Baron, Herald
};

enum class EntityType : uint8_t {
    Tower, Inhibitor, Nexus, NeutralCamp, Spawner
};

// ─── Core Position Structures ───

struct Base {
    glm::vec3 nexusPosition;
    glm::vec3 spawnPlatformCenter;
    float     spawnPlatformRadius = 8.0f;  // Fountain area
    glm::vec3 shopPosition;
};

struct Tower {
    glm::vec3                   position;
    std::optional<glm::vec3>    team2Override;     // Skip mirroring if set
    LaneType                    lane;
    TowerTier                   tier;
    float                       attackRange  = 15.0f;
    float                       maxHealth    = 3500.0f;
    float                       attackDamage = 150.0f;
};

struct Inhibitor {
    glm::vec3                   position;
    std::optional<glm::vec3>    team2Override;
    LaneType                    lane;
    float                       maxHealth    = 4000.0f;
    float                       respawnTime  = 300.0f;  // 5 minutes
};

struct Lane {
    LaneType                    type;
    std::vector<glm::vec3>      waypoints;        // Team 1 direction: base → enemy base
    float                       width = 12.0f;    // Used for "is unit in-lane?" queries
};

struct NeutralCamp {
    glm::vec3                   position;
    CampType                    campType;
    float                       spawnTime    = 90.0f;   // First spawn (seconds)
    float                       respawnTime  = 300.0f;  // Respawn interval
    float                       leashRadius  = 8.0f;    // Max chase distance
    std::vector<glm::vec3>      mobPositions;            // Individual mob offsets within camp
};

struct BrushZone {
    glm::vec3                   center;
    glm::vec3                   halfExtents;      // AABB half-size
    std::optional<glm::vec3>    team2Override;
};

struct WallSegment {
    glm::vec3 start;
    glm::vec3 end;
    float     thickness = 1.0f;
    float     height    = 3.0f;
};

// ─── Aggregate Map Data ───

struct TeamData {
    Base                        base;
    std::vector<Tower>          towers;           // Typically 11 (3 per lane + 2 nexus)
    std::vector<Inhibitor>      inhibitors;       // 3 (one per lane)
    std::array<Lane, 3>         lanes;            // Top, Mid, Bot
};

struct MapData {
    std::string                 mapName;
    std::string                 version;
    glm::vec3                   mapCenter = {100.0f, 0.0f, 100.0f};
    glm::vec3                   mapBoundsMin = {0.0f, 0.0f, 0.0f};
    glm::vec3                   mapBoundsMax = {200.0f, 20.0f, 200.0f};

    TeamData                    teams[2];         // [0] = Blue, [1] = Red (auto-generated)
    std::vector<NeutralCamp>    neutralCamps;
    std::vector<BrushZone>      brushZones;
    std::vector<WallSegment>    walls;

    // Lookup helpers
    const Lane& GetLane(TeamID team, LaneType lane) const {
        return teams[static_cast<int>(team)].lanes[static_cast<int>(lane)];
    }
};
```

### 1.2 — Symmetry System

```cpp
// ─── MapSymmetry.h ───

#pragma once
#include <glm/glm.hpp>

namespace MapSymmetry {

    // Point-reflection across map center (100, y, 100).
    // Y is preserved — height doesn't mirror.
    inline glm::vec3 MirrorPoint(const glm::vec3& point,
                                  const glm::vec3& center = {100.f, 0.f, 100.f})
    {
        return glm::vec3(
            2.0f * center.x - point.x,
            point.y,                       // Height stays the same
            2.0f * center.z - point.z
        );
    }

    // Mirror a full waypoint path and REVERSE it.
    // Team 1 path goes Base1 → Base2. Mirrored path must go Base2 → Base1.
    inline std::vector<glm::vec3> MirrorPath(const std::vector<glm::vec3>& path,
                                              const glm::vec3& center = {100.f, 0.f, 100.f})
    {
        std::vector<glm::vec3> mirrored;
        mirrored.reserve(path.size());
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
            mirrored.push_back(MirrorPoint(*it, center));
        }
        return mirrored;
    }

    // Mirror a tower, respecting optional team2Override.
    inline Tower MirrorTower(const Tower& src, const glm::vec3& center = {100.f, 0.f, 100.f}) {
        Tower mirrored = src;
        mirrored.position = src.team2Override.value_or(MirrorPoint(src.position, center));
        mirrored.team2Override = std::nullopt; // Override consumed
        return mirrored;
    }

    inline Inhibitor MirrorInhibitor(const Inhibitor& src,
                                      const glm::vec3& center = {100.f, 0.f, 100.f}) {
        Inhibitor mirrored = src;
        mirrored.position = src.team2Override.value_or(MirrorPoint(src.position, center));
        mirrored.team2Override = std::nullopt;
        return mirrored;
    }

    inline Base MirrorBase(const Base& src, const glm::vec3& center = {100.f, 0.f, 100.f}) {
        Base mirrored;
        mirrored.nexusPosition      = MirrorPoint(src.nexusPosition, center);
        mirrored.spawnPlatformCenter = MirrorPoint(src.spawnPlatformCenter, center);
        mirrored.spawnPlatformRadius = src.spawnPlatformRadius;
        mirrored.shopPosition       = MirrorPoint(src.shopPosition, center);
        return mirrored;
    }
}
```

### 1.3 — JSON Schema

```jsonc
// map_summonersrift.json
{
    "mapName": "Summoner's Rift",
    "version": "1.0.0",
    "team1": {
        "base": {
            "nexusPosition":        [22, 0, 22],
            "spawnPlatformCenter":  [15, 0, 15],
            "spawnPlatformRadius":  8.0,
            "shopPosition":         [12, 0, 12]
        },
        "towers": [
            // ── Top Lane ──
            { "position": [26, 0, 102], "lane": "Top", "tier": "Outer" },
            { "position": [28, 0, 140], "lane": "Top", "tier": "Inner" },
            { "position": [22, 0, 165], "lane": "Top", "tier": "Inhibitor" },
            // ── Mid Lane ──
            { "position": [52, 0, 52],  "lane": "Mid", "tier": "Outer" },
            { "position": [40, 0, 40],  "lane": "Mid", "tier": "Inner" },
            { "position": [30, 0, 30],  "lane": "Mid", "tier": "Inhibitor" },
            // ── Bot Lane ──
            { "position": [102, 0, 26], "lane": "Bot", "tier": "Outer" },
            { "position": [140, 0, 28], "lane": "Bot", "tier": "Inner" },
            { "position": [165, 0, 22], "lane": "Bot", "tier": "Inhibitor" },
            // ── Nexus Towers ──
            { "position": [24, 0, 28],  "lane": "Mid", "tier": "Nexus" },
            { "position": [28, 0, 24],  "lane": "Mid", "tier": "Nexus" }
        ],
        "inhibitors": [
            { "position": [22, 0, 170], "lane": "Top" },
            { "position": [28, 0, 28],  "lane": "Mid" },
            { "position": [170, 0, 22], "lane": "Bot" }
        ],
        "lanes": {
            "Top": {
                "width": 12.0,
                "waypoints": [
                    [22, 0, 22], [22, 0, 60], [22, 0, 100],
                    [22, 0, 140], [22, 0, 178], [60, 0, 178],
                    [100, 0, 178], [140, 0, 178], [178, 0, 178]
                ]
            },
            "Mid": {
                "width": 14.0,
                "waypoints": [
                    [22, 0, 22], [40, 0, 40], [60, 0, 60],
                    [80, 0, 80], [100, 0, 100], [120, 0, 120],
                    [140, 0, 140], [160, 0, 160], [178, 0, 178]
                ]
            },
            "Bot": {
                "width": 12.0,
                "waypoints": [
                    [22, 0, 22], [60, 0, 22], [100, 0, 22],
                    [140, 0, 22], [178, 0, 22], [178, 0, 60],
                    [178, 0, 100], [178, 0, 140], [178, 0, 178]
                ]
            }
        }
    },
    "neutralCamps": [
        {
            "position": [70, 0, 55],
            "campType": "BlueBuff",
            "spawnTime": 90,
            "respawnTime": 300,
            "leashRadius": 8.0,
            "mobPositions": [[0, 0, 0], [2, 0, 1], [-2, 0, 1]]
        },
        {
            "position": [55, 0, 70],
            "campType": "RedBuff",
            "spawnTime": 90,
            "respawnTime": 300,
            "leashRadius": 8.0,
            "mobPositions": [[0, 0, 0], [1.5, 0, 1.5], [-1.5, 0, 1.5]]
        },
        {
            "position": [100, 0, 65],
            "campType": "Dragon",
            "spawnTime": 300,
            "respawnTime": 360,
            "leashRadius": 12.0,
            "mobPositions": [[0, 0, 0]]
        },
        {
            "position": [100, 0, 135],
            "campType": "Baron",
            "spawnTime": 1200,
            "respawnTime": 360,
            "leashRadius": 14.0,
            "mobPositions": [[0, 0, 0]]
        }
        // ... Wolves, Raptors, Gromp, Krugs, Scuttlers ...
    ],
    "brushZones": [
        { "center": [40, 0, 120], "halfExtents": [3, 2, 8] },
        { "center": [80, 0, 90],  "halfExtents": [4, 2, 6] }
        // ...
    ],
    "walls": [
        { "start": [60, 0, 80], "end": [65, 0, 85], "thickness": 1.5, "height": 3.0 }
        // ...
    ]
}
```

### 1.4 — MapLoader Implementation

```cpp
// ─── MapLoader.h ───

#pragma once
#include "MapTypes.h"
#include <string>

class MapLoader {
public:
    // Load from JSON file path. Returns fully populated MapData with both teams.
    static MapData LoadFromFile(const std::string& filepath);

    // Validate loaded data: checks bounds, tower counts, waypoint ordering.
    static bool Validate(const MapData& data, std::string& outErrors);

private:
    static glm::vec3 ParseVec3(const nlohmann::json& j);
    static LaneType  ParseLaneType(const std::string& str);
    static TowerTier ParseTowerTier(const std::string& str);
    static CampType  ParseCampType(const std::string& str);

    static TeamData  ParseTeam1(const nlohmann::json& j);
    static void      GenerateTeam2(MapData& data);  // Mirrors Team 1 → Team 2
};
```

**Key implementation behaviors of `GenerateTeam2()`:**
1. Mirrors `Base` via `MapSymmetry::MirrorBase`.
2. Iterates all towers — for each, checks `team2Override`; if present, uses it; otherwise calls `MirrorPoint`.
3. Mirrors all inhibitors with the same override logic.
4. For each lane, mirrors the waypoint vector using `MirrorPath` (which reverses order).
5. Does NOT touch `neutralCamps` — those are loaded as-is for both teams.
6. Mirrors `brushZones` using the same override pattern.

**Validation checks (`Validate()`):**
- All positions within map bounds `(0–200, 0–maxHeight, 0–200)`.
- Exactly 11 towers per team (3 per lane × 3 tiers + 2 nexus towers).
- Exactly 3 inhibitors per team (one per lane).
- Waypoints ordered correctly (first waypoint near own base, last near enemy base).
- No duplicate positions within a tolerance of 0.5 units.
- Neutral camp positions not overlapping with tower positions.

### Phase 1 Deliverables
- [ ] `MapTypes.h` compiles with no warnings.
- [ ] `map_summonersrift.json` parses without error.
- [ ] `MapLoader::LoadFromFile()` produces a `MapData` with Team 2 auto-generated.
- [ ] `MapLoader::Validate()` passes all checks.
- [ ] Unit test: mirror of `(30, 0, 40)` → `(170, 0, 160)`. Mirror of mirror → original.
- [ ] Unit test: mirrored lane waypoints are in reverse order.

---

## Phase 2: Vulkan Terrain Generation

### Goal
Generate a subdivided terrain mesh from a heightmap, split into chunks for efficient culling, with correct normals and tangents for later normal mapping.

### 2.1 — Vertex Format

```cpp
struct TerrainVertex {
    glm::vec3 pos;       // World-space position
    glm::vec2 uv;        // Texture coordinate (0–1 across full map)
    glm::vec3 normal;    // Surface normal (computed from neighbors)
    glm::vec3 tangent;   // Tangent vector (for normal mapping)
};

// Vulkan vertex input description:
// Binding 0, stride = sizeof(TerrainVertex)
// Location 0: offset 0,  R32G32B32_SFLOAT    (pos)
// Location 1: offset 12, R32G32_SFLOAT       (uv)
// Location 2: offset 20, R32G32B32_SFLOAT    (normal)
// Location 3: offset 32, R32G32B32_SFLOAT    (tangent)
// Total: 44 bytes per vertex
```

### 2.2 — Heightmap Loading & Mesh Generation

**Input:** A 256×256 grayscale PNG (8-bit, single channel).

**Parameters:**

| Parameter      | Value   | Description                                      |
|----------------|---------|--------------------------------------------------|
| `mapWidth`     | 200.0   | World units along X                              |
| `mapDepth`     | 200.0   | World units along Z                              |
| `maxHeight`    | 10.0    | Maximum Y displacement at pixel value 255        |
| `hmResolution` | 256     | Heightmap pixel dimensions (square)              |
| `chunkSize`    | 32      | Vertices per chunk edge (8×8 grid of chunks)     |

**Algorithm:**

```
For each pixel (ix, iz) in the heightmap:
    heightNormalized = pixel(ix, iz) / 255.0

    vertex.pos.x = (ix / (hmResolution - 1)) * mapWidth
    vertex.pos.z = (iz / (hmResolution - 1)) * mapDepth
    vertex.pos.y = heightNormalized * maxHeight

    vertex.uv.x = ix / (hmResolution - 1)
    vertex.uv.y = iz / (hmResolution - 1)
```

**Normal Calculation (Finite Differences):**

```
For each pixel (ix, iz):
    hL = height(ix - 1, iz)    // clamped at edges
    hR = height(ix + 1, iz)
    hD = height(ix, iz - 1)
    hU = height(ix, iz + 1)

    normal = normalize(vec3(hL - hR, 2.0 * stepSize, hD - hU))
```

Where `stepSize = mapWidth / (hmResolution - 1)`.

**Tangent Calculation:**

```
For each vertex:
    tangent = normalize(cross(vec3(0, 1, 0), normal))
    if (length(tangent) < 0.001)   // Normal is straight up
        tangent = vec3(1, 0, 0)
```

**Index Buffer Generation:**

```
For each quad (ix, iz) where ix < hmResolution-1, iz < hmResolution-1:
    topLeft     = iz * hmResolution + ix
    topRight    = topLeft + 1
    bottomLeft  = (iz + 1) * hmResolution + ix
    bottomRight = bottomLeft + 1

    Triangle 1: topLeft, bottomLeft, topRight
    Triangle 2: topRight, bottomLeft, bottomRight
```

Total indices: `(255 × 255 × 6)` = 390,150 for the full map.

### 2.3 — Chunk System

**Why chunks:** At the isometric camera angle, roughly 40–60% of the map is outside the view frustum. Culling entire chunks avoids submitting their draw calls.

**Chunk layout:** 8×8 grid = 64 chunks, each covering a 32×32 vertex region (25×25 world units).

**Per chunk, store:**

```cpp
struct TerrainChunk {
    VkBuffer        vertexBuffer;
    VkBuffer        indexBuffer;
    VmaAllocation   vertexAlloc;
    VmaAllocation   indexAlloc;
    uint32_t        indexCount;
    glm::vec3       aabbMin;         // Axis-aligned bounding box
    glm::vec3       aabbMax;
    bool            visible = true;  // Set by frustum cull each frame
};
```

**Frustum Culling (per frame, CPU-side):**

```
Extract 6 frustum planes from ViewProjection matrix.
For each chunk:
    Test chunk AABB against all 6 planes.
    If fully outside any plane → chunk.visible = false.
    Otherwise → chunk.visible = true.
```

Only submit `vkCmdDrawIndexed` for visible chunks.

### 2.4 — Height Querying (CPU-Side)

Other systems (navigation, entity placement) need to query terrain height at arbitrary `(x, z)` positions.

```cpp
// Returns the interpolated terrain height at world position (x, z).
float TerrainSystem::GetHeightAt(float x, float z) const;
```

**Implementation:** Bilinear interpolation of the 4 nearest heightmap pixels. Store the heightmap data in a CPU-side `std::vector<float>` after loading — don't read back from the GPU.

### 2.5 — Isometric Camera

**Camera Parameters:**

| Parameter        | Value           | Notes                                            |
|------------------|-----------------|--------------------------------------------------|
| Pitch            | 56°             | Angle from horizontal (League-accurate)          |
| Yaw              | 225° (or -135°) | Camera looks from top-right toward bottom-left   |
| Projection       | Perspective     | FOV = 30°, near = 1.0, far = 500.0              |
| Target           | Map center      | (100, 0, 100) initially, follows player          |
| Distance         | 120 units       | From target along the reversed forward vector    |

**View Matrix Calculation:**

```cpp
glm::mat4 CalculateIsometricView(glm::vec3 target, float distance,
                                  float pitchDeg, float yawDeg)
{
    float pitchRad = glm::radians(pitchDeg);
    float yawRad   = glm::radians(yawDeg);

    glm::vec3 direction;
    direction.x = cos(pitchRad) * cos(yawRad);
    direction.y = sin(pitchRad);
    direction.z = cos(pitchRad) * sin(yawRad);

    glm::vec3 eye = target + direction * distance;
    return glm::lookAt(eye, target, glm::vec3(0, 1, 0));
}
```

**Camera Bounds Clamping:**

```cpp
// Clamp target so the visible area never extends beyond map edges.
target.x = glm::clamp(target.x, margin, 200.0f - margin);
target.z = glm::clamp(target.z, margin, 200.0f - margin);
// 'margin' depends on FOV and distance — compute from projected screen edges.
```

**Camera Scroll:** Mouse-edge panning (when cursor is within 20px of screen edge) at a configurable speed (default: 30 units/sec). Middle-mouse drag for immediate repositioning. Scroll wheel adjusts `distance` between `[80, 160]` for zoom.

### 2.6 — Terrain Vulkan Pipeline

**Pipeline configuration:**

- Vertex shader: transforms via MVP, passes world pos + UV + normal + tangent to fragment.
- Fragment shader: placeholder solid color (0.2, 0.5, 0.1) — real shading comes in Phase 3.
- Polygon mode: `VK_POLYGON_MODE_FILL` (with a debug toggle for wireframe).
- Cull mode: `VK_CULL_MODE_BACK_BIT`.
- Depth test: enabled, `VK_COMPARE_OP_LESS`.
- Depth write: enabled.

**Vertex Shader (terrain.vert):**

```glsl
#version 450

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    float time;
};

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec3 inTangent;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec3 fragTangent;

void main() {
    // Terrain is in world space — no model matrix needed.
    fragWorldPos = inPos;
    fragUV       = inUV;
    fragNormal   = inNormal;
    fragTangent  = inTangent;

    gl_Position = proj * view * vec4(inPos, 1.0);
}
```

### Phase 2 Deliverables
- [ ] Heightmap loads and generates a visible mesh — green terrain on screen.
- [ ] Wireframe toggle shows correct triangulation.
- [ ] Camera orbits correctly at the isometric angle; scrolling/zooming works.
- [ ] Frustum culling active — verify by logging visible chunk count.
- [ ] `GetHeightAt()` returns correct values — test by spawning debug spheres at known positions.
- [ ] No Z-fighting or depth buffer artifacts.

---

## Phase 3: Texture Splatting & Water

### Goal
Replace the solid green terrain with a multi-textured surface using a splat map, add a separate water plane for the river, and apply team-colored territory tinting.

### 3.1 — Required Textures

| Slot      | Descriptor Binding | Resolution | Format          | Source                       |
|-----------|--------------------|------------|-----------------|------------------------------|
| Grass     | Set 1, Binding 0   | 1024×1024  | `R8G8B8A8_SRGB` | Tiling grass texture         |
| Path/Dirt | Set 1, Binding 1   | 1024×1024  | `R8G8B8A8_SRGB` | Tiling dirt/stone path       |
| Stone     | Set 1, Binding 2   | 1024×1024  | `R8G8B8A8_SRGB` | Tiling rock texture          |
| Splat Map | Set 1, Binding 3   | 512×512    | `R8G8B8A8_UNORM`| Hand-painted (R=Grass, G=Path, B=Stone, A=Water) |
| Team Map  | Set 1, Binding 4   | 256×256    | `R8_UNORM`      | 0.0 = full Blue, 1.0 = full Red |
| Normal Map| Set 1, Binding 5   | 1024×1024  | `R8G8B8A8_UNORM`| Terrain detail normal map    |

**Splat map channel usage:**
- **R** = Grass weight
- **G** = Dirt/Path weight
- **B** = Stone weight
- **A** = Water mask (used to darken terrain under water plane and block grass)

### 3.2 — Terrain Fragment Shader (terrain.frag)

```glsl
#version 450

// ─── Inputs from Vertex Shader ───
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragTangent;

// ─── Uniforms ───
layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    float time;
};

// ─── Textures ───
layout(set = 1, binding = 0) uniform sampler2D texGrass;
layout(set = 1, binding = 1) uniform sampler2D texPath;
layout(set = 1, binding = 2) uniform sampler2D texStone;
layout(set = 1, binding = 3) uniform sampler2D texSplatMap;
layout(set = 1, binding = 4) uniform sampler2D texTeamMap;
layout(set = 1, binding = 5) uniform sampler2D texNormalMap;

layout(location = 0) out vec4 outColor;

// ─── Constants ───
const float TILE_SCALE    = 20.0;       // How many times textures tile across the map
const vec3  SUN_DIR       = normalize(vec3(0.4, 0.8, 0.3));
const vec3  SUN_COLOR     = vec3(1.0, 0.95, 0.85);
const vec3  AMBIENT       = vec3(0.15, 0.17, 0.22);
const vec3  BLUE_TINT     = vec3(0.4, 0.5, 0.9);
const vec3  RED_TINT      = vec3(0.9, 0.4, 0.4);
const float TEAM_TINT_STR = 0.08;       // Subtle — 8% blend
const float WATER_DARKEN  = 0.4;        // How much terrain darkens under water

void main() {
    // ─── Splat Map Sampling ───
    vec4 splat = texture(texSplatMap, fragUV);
    // Normalize weights so they sum to 1.0 (handles hand-painting imprecision)
    float totalWeight = splat.r + splat.g + splat.b;
    if (totalWeight > 0.001) {
        splat.rgb /= totalWeight;
    } else {
        splat.r = 1.0;  // Default to grass
    }

    // ─── Tiled Texture Sampling ───
    vec2 tiledUV  = fragUV * TILE_SCALE;
    vec3 grass    = texture(texGrass, tiledUV).rgb;
    vec3 path     = texture(texPath,  tiledUV).rgb;
    vec3 stone    = texture(texStone, tiledUV).rgb;

    // ─── Blend ───
    vec3 baseColor = grass * splat.r + path * splat.g + stone * splat.b;

    // ─── Water Mask (from splat alpha) ───
    float waterInfluence = splat.a;
    baseColor = mix(baseColor, baseColor * WATER_DARKEN, waterInfluence);

    // ─── Normal Mapping ───
    vec3 N = normalize(fragNormal);
    vec3 T = normalize(fragTangent);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    vec3 detailNormal = texture(texNormalMap, tiledUV).rgb * 2.0 - 1.0;
    vec3 worldNormal = normalize(TBN * detailNormal);

    // ─── Lighting (Simple Directional) ───
    float NdotL = max(dot(worldNormal, SUN_DIR), 0.0);
    vec3 diffuse = SUN_COLOR * NdotL;
    vec3 lit = baseColor * (AMBIENT + diffuse);

    // ─── Team Territory Tint ───
    float teamValue = texture(texTeamMap, fragUV).r;  // 0 = Blue, 1 = Red
    vec3 teamColor = mix(BLUE_TINT, RED_TINT, teamValue);
    lit = mix(lit, teamColor, TEAM_TINT_STR);

    outColor = vec4(lit, 1.0);
}
```

### 3.3 — Separate Water Plane

The river is NOT painted terrain — it is a separate transparent mesh rendered after the terrain.

**Water Mesh:** A single quad (two triangles) placed at the river height (e.g., `Y = 0.8`). The quad covers the river region. For a simple approach, use a full-map quad and discard fragments where the splat map alpha is 0.

**Water Vertex Shader:** Same as terrain vertex shader but uses a dedicated model matrix to position the water plane.

**Water Fragment Shader (water.frag):**

```glsl
#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec2 fragUV;

layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    vec4 cameraPos;
    float time;
};

layout(set = 1, binding = 0) uniform sampler2D texSplatMap;  // To mask river area
layout(set = 1, binding = 1) uniform sampler2D texWaterNormal;

layout(location = 0) out vec4 outColor;

const vec3  WATER_COLOR   = vec3(0.1, 0.2, 0.5);
const vec3  WATER_SHALLOW = vec3(0.15, 0.35, 0.5);
const float FLOW_SPEED    = 0.03;
const float WATER_ALPHA   = 0.65;

void main() {
    // Only render in river area
    float riverMask = texture(texSplatMap, fragUV).a;
    if (riverMask < 0.05) discard;

    // Scrolling UV for flow effect
    vec2 flowUV1 = fragUV * 8.0 + vec2(time * FLOW_SPEED,  time * FLOW_SPEED * 0.7);
    vec2 flowUV2 = fragUV * 6.0 + vec2(-time * FLOW_SPEED * 0.5, time * FLOW_SPEED * 0.3);

    // Two scrolling normal samples, blended for ripple
    vec3 n1 = texture(texWaterNormal, flowUV1).rgb * 2.0 - 1.0;
    vec3 n2 = texture(texWaterNormal, flowUV2).rgb * 2.0 - 1.0;
    vec3 waterNormal = normalize(n1 + n2);

    // Simple specular highlight from sun
    vec3 viewDir  = normalize(cameraPos.xyz - fragWorldPos);
    vec3 sunDir   = normalize(vec3(0.4, 0.8, 0.3));
    vec3 halfDir  = normalize(viewDir + sunDir);
    float spec    = pow(max(dot(waterNormal, halfDir), 0.0), 64.0);

    vec3 color = mix(WATER_COLOR, WATER_SHALLOW, riverMask) + vec3(spec * 0.5);

    outColor = vec4(color, WATER_ALPHA * riverMask);
}
```

**Water Pipeline Configuration:**
- Blend mode: `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` (standard alpha blend).
- Depth test: enabled (read-only — `depthWriteEnable = VK_FALSE`).
- Cull mode: `VK_CULL_MODE_NONE` (visible from both sides if camera dips).
- Render order: AFTER terrain (opaque first, then transparent).

### Phase 3 Deliverables
- [ ] Terrain shows grass, dirt, and stone blended correctly by splat map.
- [ ] Water plane renders with scrolling flow, visible specular highlight.
- [ ] River area under water is darkened on the terrain.
- [ ] Blue/Red team territory tint is visible but subtle.
- [ ] Normal mapping adds visual detail without geometric cost.
- [ ] No alpha-sorting artifacts on the water plane.

---

## Phase 4: Navigation & Entity Spawning

### Goal
Implement smooth minion pathing along lanes, a debug visualization system, and a structured entity spawning pipeline.

### 4.1 — Catmull-Rom Spline Interpolation

**Why Catmull-Rom:** It passes through all control points (unlike Bezier), which means your level designer's waypoints are exactly where the path goes. It also provides C1 continuity (smooth tangents).

```cpp
// ─── SplineUtil.h ───

#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace SplineUtil {

    // Catmull-Rom interpolation between p1 and p2 using p0 and p3 as tangent guides.
    // t ranges from 0.0 (at p1) to 1.0 (at p2).
    inline glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1,
                                 const glm::vec3& p2, const glm::vec3& p3,
                                 float t, float alpha = 0.5f)
    {
        float t2 = t * t;
        float t3 = t2 * t;

        // Standard Catmull-Rom basis with configurable tension (alpha = 0.5 is standard)
        glm::vec3 result =
            (2.0f * p1) +
            (-p0 + p2) * t +
            (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3;

        return result * 0.5f;
    }

    // Returns a point on the full lane path.
    // globalT ranges from 0.0 (lane start) to 1.0 (lane end).
    glm::vec3 GetPointOnLane(const std::vector<glm::vec3>& waypoints, float globalT);

    // Returns the tangent (direction) at a point on the lane.
    glm::vec3 GetTangentOnLane(const std::vector<glm::vec3>& waypoints, float globalT);

    // Converts total arc length → approximate globalT.
    // Precomputed using a lookup table for accuracy.
    float ArcLengthToT(const std::vector<glm::vec3>& waypoints, float distance);
}
```

**`GetPointOnLane` implementation logic:**

```
Given N waypoints and globalT in [0, 1]:
    numSegments = N - 1
    scaledT     = globalT * numSegments
    segment     = floor(scaledT)         // Which segment (0 to N-2)
    localT      = scaledT - segment      // Progress within segment (0 to 1)

    // Clamp segment to valid range
    segment = clamp(segment, 0, numSegments - 1)

    // Get 4 control points (clamp at edges)
    p0 = waypoints[max(0, segment - 1)]
    p1 = waypoints[segment]
    p2 = waypoints[min(N-1, segment + 1)]
    p3 = waypoints[min(N-1, segment + 2)]

    return CatmullRom(p0, p1, p2, p3, localT)
```

### 4.2 — Lane Navigation Component

```cpp
struct LaneFollower {
    TeamID      team;
    LaneType    lane;
    float       progress     = 0.0f;     // 0 = own base, 1 = enemy base
    float       moveSpeed    = 6.0f;     // Units per second
    glm::vec3   currentPos;
    glm::vec3   targetPos;               // For deviation (aggro/combat)
    bool        isDeviating  = false;    // True when chasing a target off-lane
    float       returnTimer  = 0.0f;     // Time before snapping back to lane

    // After deviation, store the lane return position.
    glm::vec3   laneReturnPos;
};
```

**Update logic (per frame):**

```
if (!isDeviating):
    progress += (moveSpeed * dt) / totalLaneArcLength
    progress = clamp(progress, 0, 1)
    currentPos = GetPointOnLane(waypoints, progress)
    currentPos.y = terrain.GetHeightAt(currentPos.x, currentPos.z)  // Stick to ground
else:
    // Move toward targetPos
    // If reached target or returnTimer expired:
    //   isDeviating = false
    //   Find closest globalT on lane to current position
    //   Resume lane following from there
```

### 4.3 — Debug Rendering System

**Purpose:** Visualize lanes, tower ranges, brush zones, waypoints, and camp leash radii during development. Must be toggleable and have zero cost when disabled.

**Debug Line Pipeline:**

```cpp
struct DebugVertex {
    glm::vec3 pos;
    glm::vec4 color;
};

class DebugRenderer {
public:
    void Init(VulkanContext& ctx);
    void Shutdown();

    // Call during game update — buffers draw commands
    void DrawLine(glm::vec3 a, glm::vec3 b, glm::vec4 color);
    void DrawSphere(glm::vec3 center, float radius, glm::vec4 color, int segments = 12);
    void DrawAABB(glm::vec3 min, glm::vec3 max, glm::vec4 color);
    void DrawCircle(glm::vec3 center, float radius, glm::vec4 color, int segments = 32);

    // Call during render — flushes all buffered draws
    void Render(VkCommandBuffer cmd, const glm::mat4& viewProj);
    void Clear();  // Call at start of each frame

private:
    std::vector<DebugVertex>    m_vertices;
    VkBuffer                    m_vertexBuffer;      // Dynamic, recreated if exceeded
    VmaAllocation               m_vertexAlloc;
    VkPipeline                  m_pipeline;           // LINE_LIST topology
    VkPipelineLayout            m_pipelineLayout;
    size_t                      m_maxVertices = 100000;
    bool                        m_enabled = true;
};
```

**Pipeline configuration:**
- Topology: `VK_PRIMITIVE_TOPOLOGY_LINE_LIST`.
- Line width: 2.0f (check `VkPhysicalDeviceFeatures::wideLines`).
- Depth test: enabled (read-only — lines render behind/in-front correctly).
- No backface culling.
- Blending: enabled (for semi-transparent range circles).

**What to visualize:**

| Element        | Shape        | Color                | Condition          |
|----------------|-------------|----------------------|--------------------|
| Lane waypoints | Spheres (r=0.5) | Yellow            | Always             |
| Lane curves    | Line strips  | White (per lane)    | Always             |
| Tower positions | Spheres (r=1.0) | Blue/Red per team | Always             |
| Tower range    | Circles      | Orange              | When selected       |
| Inhibitors     | Spheres (r=1.2) | Purple            | Always             |
| Nexus          | Sphere (r=2.0) | Gold               | Always             |
| Brush zones    | AABBs        | Green transparent   | Always             |
| Camp leash     | Circles      | Red                 | Always             |
| Walls          | Line segments | White thick         | Always             |
| Chunk AABBs    | AABBs        | Gray                | On toggle           |

### 4.4 — Entity Spawning System

```cpp
// ─── SpawnSystem.h ───

#pragma once
#include "MapTypes.h"
#include <vector>

enum class SpawnEntityType : uint8_t {
    Tower, Inhibitor, Nexus, NeutralCamp, SpawnPlatform, Shop
};

struct SpawnCommand {
    SpawnEntityType     type;
    TeamID              team;
    glm::vec3           position;
    glm::vec3           rotation = {0, 0, 0};  // Euler angles (towers face lane center)
    float               scale    = 1.0f;

    // Gameplay data
    std::string         meshId;          // Key into mesh asset registry
    float               maxHealth  = 0;
    float               attackRange = 0;
    float               attackDamage = 0;
    LaneType            lane = LaneType::Mid;
    TowerTier           tier = TowerTier::Outer;
    CampType            campType = CampType::Dragon;

    // Metadata
    std::string         debugName;       // e.g., "Blue_Top_OuterTower"
};

class SpawnSystem {
public:
    // Reads MapData and produces a flat list of spawn commands.
    static std::vector<SpawnCommand> GenerateSpawnCommands(const MapData& mapData);

private:
    // Computes tower facing direction (toward the lane center it guards).
    static glm::vec3 ComputeTowerRotation(const Tower& tower, const MapData& mapData);

    // Assigns mesh IDs based on entity type and team.
    static std::string ResolveMeshId(SpawnEntityType type, TeamID team, TowerTier tier);
};
```

**`GenerateSpawnCommands` pseudocode:**

```
commands = []

for each team in [Blue, Red]:
    // Nexus
    cmd = { type=Nexus, team, pos=base.nexusPosition, mesh="nexus_blue/red" }
    commands.push(cmd)

    // Spawn Platform (visual only — the fountain area)
    cmd = { type=SpawnPlatform, team, pos=base.spawnPlatformCenter, scale=base.radius }
    commands.push(cmd)

    // Towers
    for each tower in team.towers:
        cmd = {
            type=Tower, team, pos=tower.position,
            rotation=ComputeTowerRotation(tower),
            maxHealth=tower.maxHealth, attackRange=tower.attackRange,
            attackDamage=tower.attackDamage, lane=tower.lane, tier=tower.tier,
            mesh=ResolveMeshId(Tower, team, tier),
            debugName=FormatName(team, lane, tier)  // "Blue_Top_Outer"
        }
        commands.push(cmd)

    // Inhibitors
    for each inhib in team.inhibitors:
        cmd = { type=Inhibitor, team, pos=inhib.position, maxHealth=inhib.maxHealth, ... }
        commands.push(cmd)

// Neutral Camps
for each camp in neutralCamps:
    cmd = { type=NeutralCamp, team=Neutral, pos=camp.position, campType=camp.campType, ... }
    commands.push(cmd)

return commands
```

### 4.5 — Entity Instantiation

The engine's ECS (or object system) consumes `SpawnCommand` and creates runtime entities:

```cpp
void GameWorld::InstantiateFromSpawnCommands(const std::vector<SpawnCommand>& commands)
{
    for (const auto& cmd : commands) {
        Entity entity = CreateEntity();

        // Transform
        entity.AddComponent<Transform>({
            .position = cmd.position,
            .rotation = cmd.rotation,
            .scale    = glm::vec3(cmd.scale)
        });

        // Renderable
        entity.AddComponent<MeshRenderer>({
            .meshId = cmd.meshId,
            .teamColor = (cmd.team == TeamID::Blue) ? BLUE : RED
        });

        // Gameplay (type-dependent)
        switch (cmd.type) {
            case SpawnEntityType::Tower:
                entity.AddComponent<Health>({ cmd.maxHealth });
                entity.AddComponent<TowerAI>({ cmd.attackRange, cmd.attackDamage });
                entity.AddComponent<LaneAssignment>({ cmd.lane, cmd.tier });
                break;
            case SpawnEntityType::Inhibitor:
                entity.AddComponent<Health>({ cmd.maxHealth });
                entity.AddComponent<Respawnable>({ 300.0f });
                break;
            case SpawnEntityType::NeutralCamp:
                entity.AddComponent<CampController>({ cmd.campType });
                break;
            // ...
        }
    }
}
```

### Phase 4 Deliverables
- [ ] Spline interpolation produces visually smooth curves through all waypoints.
- [ ] Debug renderer draws lane paths, tower positions, and range circles.
- [ ] A test minion entity follows the Mid lane from `t=0` to `t=1` over time.
- [ ] All towers, inhibitors, and nexuses appear at correct positions for both teams.
- [ ] `SpawnCommand` list matches expected count (22 towers, 6 inhibitors, 2 nexuses, N camps).
- [ ] Tower rotations face toward the lane they guard.

---

## Phase 5: Fog of War

### Goal
Implement a basic fog of war system that hides areas of the map not currently visible to the player's team. This is a core MOBA mechanic — without it, the game has no information asymmetry.

### 5.1 — Architecture

**Approach: GPU-side visibility texture + post-process darkening.**

The fog system maintains a low-resolution 2D texture (the "vision map") that represents which areas are currently visible. Each frame, the CPU updates this texture based on friendly unit positions and sight ranges. A full-screen post-process shader reads this texture and darkens areas that are not visible.

```
┌──────────────────────────┐
│ CPU: Vision Update       │
│  For each friendly unit: │
│    Paint circle on       │     Upload
│    vision map buffer     │ ──────────► VisionTexture (128×128)
└──────────────────────────┘                    │
                                                ▼
                                    ┌─────────────────────┐
                                    │ Post-Process Shader  │
                                    │  Sample scene color  │
                                    │  Sample vision map   │
                                    │  Darken if not visible│
                                    └─────────────────────┘
```

### 5.2 — Vision Map

**Resolution:** 128×128 (each texel covers ~1.56×1.56 world units — sufficient for MOBA gameplay).

**Format:** `VK_FORMAT_R8_UNORM` — single channel, 0 = fully fogged, 255 = fully visible.

**CPU-side buffer:** `std::vector<uint8_t> visionBuffer(128 * 128, 0)`.

**Per-frame update:**

```
Clear visionBuffer to 0 (all fogged)

For each entity on friendlyTeam:
    mapX = entity.pos.x / 200.0 * 128.0
    mapZ = entity.pos.z / 200.0 * 128.0
    sightRadiusPixels = entity.sightRange / 200.0 * 128.0

    // Paint a filled circle on the vision buffer
    For each pixel (px, pz) in bounding box of circle:
        if distance((px, pz), (mapX, mapZ)) <= sightRadiusPixels:
            // Apply soft falloff at edges
            falloff = 1.0 - smoothstep(sightRadiusPixels * 0.8, sightRadiusPixels, dist)
            visionBuffer[pz * 128 + px] = max(current, uint8_t(falloff * 255))

Upload visionBuffer to VisionTexture via staging buffer
```

**Line-of-sight occlusion (optional, advanced):** Cast rays from the unit position against wall segments. If a wall is between the unit and a pixel, reduce or zero that pixel's visibility. This is expensive at 128×128 and can be deferred to a later optimization pass.

### 5.3 — "Previously Explored" Memory

Standard fog of war has three states:
1. **Visible** — currently seen by a friendly unit (full color).
2. **Previously explored** — was seen before but no unit currently sees it (dimmed, shows terrain but not enemies).
3. **Unexplored** — never seen (fully dark).

Implement a second 128×128 texture: the **exploration map**. This texture is never cleared — once a pixel is set to 255, it stays. Each frame:

```
For each pixel:
    explorationMap[i] = max(explorationMap[i], visionMap[i])
```

### 5.4 — Post-Process Shader (fog.frag)

This requires rendering the 3D scene to an offscreen framebuffer first, then running a full-screen quad that composites the fog.

**Modified Render Pass Structure:**

```
Pass 1 (Offscreen): Render terrain + water + entities → Color texture + Depth texture
Pass 2 (Post-process): Full-screen quad, samples Pass 1 color + vision textures → Swapchain
```

```glsl
#version 450

layout(location = 0) in vec2 fragUV;  // Screen-space UV (0–1)

layout(set = 0, binding = 0) uniform sampler2D sceneColor;       // Pass 1 output
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;       // Pass 1 depth
layout(set = 0, binding = 2) uniform sampler2D visionMap;        // Current visibility
layout(set = 0, binding = 3) uniform sampler2D explorationMap;   // Historical visibility

layout(set = 0, binding = 4) uniform FogUBO {
    mat4  invViewProj;      // To reconstruct world position from depth
    vec4  fogColorExplored; // e.g., (0.0, 0.0, 0.0, 0.5) — dim
    vec4  fogColorUnknown;  // e.g., (0.0, 0.0, 0.0, 0.95) — nearly black
};

layout(location = 0) out vec4 outColor;

vec3 ReconstructWorldPos(vec2 uv, float depth) {
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldPos = invViewProj * clipPos;
    return worldPos.xyz / worldPos.w;
}

void main() {
    vec4 scene = texture(sceneColor, fragUV);
    float depth = texture(sceneDepth, fragUV).r;

    // Reconstruct world XZ to sample fog maps
    vec3 worldPos = ReconstructWorldPos(fragUV, depth);
    vec2 fogUV = vec2(worldPos.x / 200.0, worldPos.z / 200.0);
    fogUV = clamp(fogUV, 0.0, 1.0);

    float visibility  = texture(visionMap, fogUV).r;
    float explored    = texture(explorationMap, fogUV).r;

    // Three-state fog
    if (visibility > 0.1) {
        // Currently visible — full color, slight vignette at edge of vision
        float edge = smoothstep(0.1, 0.4, visibility);
        outColor = mix(vec4(scene.rgb * 0.7, 1.0), scene, edge);
    } else if (explored > 0.1) {
        // Previously explored — dimmed, desaturated
        vec3 gray = vec3(dot(scene.rgb, vec3(0.299, 0.587, 0.114)));
        vec3 dimmed = mix(gray, scene.rgb, 0.3) * 0.5;
        outColor = vec4(dimmed, 1.0);
    } else {
        // Unexplored — near black
        outColor = fogColorUnknown;
    }
}
```

### 5.5 — Entity Visibility

Fog of war also affects which enemy entities are rendered:

```cpp
void FogSystem::UpdateEntityVisibility(const std::vector<Entity>& enemies,
                                        const uint8_t* visionBuffer)
{
    for (auto& enemy : enemies) {
        int px = int(enemy.pos.x / 200.0f * 128.0f);
        int pz = int(enemy.pos.z / 200.0f * 128.0f);
        px = std::clamp(px, 0, 127);
        pz = std::clamp(pz, 0, 127);

        uint8_t vis = visionBuffer[pz * 128 + px];
        enemy.isVisibleToPlayer = (vis > 25);  // Small threshold to avoid flicker
    }
}
```

Entities with `isVisibleToPlayer = false` are skipped during the entity rendering pass. They still simulate on the server/authoritative side — they're just not drawn.

### 5.6 — Performance Considerations

- Vision map upload is 128×128 × 1 byte = 16 KB per frame — negligible.
- The post-process pass is a single full-screen quad — one draw call.
- The CPU circle-painting loop handles ~50 units comfortably. For 200+ units (unlikely in a MOBA), switch to a compute shader approach.
- Use `VK_FILTER_LINEAR` on the vision map sampler — the bilinear interpolation gives free edge-softening.

### Phase 5 Deliverables
- [ ] Fog of war renders with all three states (visible, explored, unexplored).
- [ ] Moving a friendly unit reveals terrain around it in real time.
- [ ] Explored areas remain dimmed after the unit leaves.
- [ ] Enemy entities disappear when outside friendly vision.
- [ ] Performance: fog system adds < 0.5ms per frame (profile with GPU timestamps).
- [ ] Vision correctly wraps around brush zones (units inside brush are only visible at close range — integrate with brush zone data from Phase 1).

---

## Implementation Order & Dependencies

```
Phase 0 ──► Phase 1 ──► Phase 2 ──► Phase 3 ──► Phase 4 ──► Phase 5
  │              │           │           │           │           │
  │              │           │           │           │           │
Vulkan       MapData     Terrain     Shading     Navigation   Fog of
bootstrap    + JSON      + Camera    + Water     + Spawning   War
             loading     + Height    + Team tint + Debug vis
                         query
```

**Key dependency chains:**
- Phase 2 depends on Phase 0 (needs Vulkan rendering) and Phase 1 (needs map bounds).
- Phase 3 depends on Phase 2 (needs terrain mesh and vertex shader outputs).
- Phase 4 depends on Phase 1 (needs waypoints and entity positions) and Phase 2 (needs `GetHeightAt` for ground clamping).
- Phase 5 depends on Phase 2 (needs depth buffer and scene rendering) and Phase 4 (needs entity positions for vision sources).
- Phase 4's debug renderer can be tested independently once Phase 0 is complete.

**Estimated time (solo developer, engine from scratch):**

| Phase   | Estimated Time | Cumulative |
|---------|---------------|------------|
| Phase 0 | 2–3 weeks     | 3 weeks    |
| Phase 1 | 3–5 days      | ~4 weeks   |
| Phase 2 | 1–2 weeks     | ~6 weeks   |
| Phase 3 | 1 week        | ~7 weeks   |
| Phase 4 | 1–2 weeks     | ~9 weeks   |
| Phase 5 | 1 week        | ~10 weeks  |

**Milestone checkpoints:**
- **Week 3:** Window renders, Vulkan validation clean, frame loop stable.
- **Week 4:** JSON loads, Team 2 mirrors correctly, all unit tests pass.
- **Week 6:** Terrain visible from isometric camera, heightmap deformation working.
- **Week 7:** Multi-texture terrain with water plane looks recognizably like a MOBA map.
- **Week 9:** Debug lines show lanes, towers appear at correct positions, test minion walks a lane.
- **Week 10:** Fog of war active, full map playable for camera exploration and basic entity viewing.

---

## File Structure

```
project/
├── assets/
│   ├── maps/
│   │   └── map_summonersrift.json
│   ├── textures/
│   │   ├── heightmap.png              (256×256 grayscale)
│   │   ├── splatmap.png               (512×512 RGBA)
│   │   ├── teammap.png                (256×256 grayscale)
│   │   ├── grass.png
│   │   ├── dirt.png
│   │   ├── stone.png
│   │   ├── water_normal.png
│   │   └── terrain_normal.png
│   └── shaders/
│       ├── terrain.vert
│       ├── terrain.frag
│       ├── water.vert
│       ├── water.frag
│       ├── debug.vert
│       ├── debug.frag
│       ├── fog.vert                   (fullscreen quad)
│       └── fog.frag
├── src/
│   ├── core/
│   │   ├── VulkanContext.h / .cpp     (Phase 0)
│   │   ├── Swapchain.h / .cpp
│   │   ├── BufferBuilder.h / .cpp
│   │   └── DescriptorManager.h / .cpp
│   ├── map/
│   │   ├── MapTypes.h                 (Phase 1)
│   │   ├── MapSymmetry.h
│   │   ├── MapLoader.h / .cpp
│   │   └── MapValidator.h / .cpp
│   ├── terrain/
│   │   ├── TerrainGenerator.h / .cpp  (Phase 2)
│   │   ├── TerrainChunk.h / .cpp
│   │   ├── HeightQuery.h / .cpp
│   │   └── IsometricCamera.h / .cpp
│   ├── rendering/
│   │   ├── TerrainPipeline.h / .cpp   (Phase 2–3)
│   │   ├── WaterPipeline.h / .cpp     (Phase 3)
│   │   ├── DebugRenderer.h / .cpp     (Phase 4)
│   │   └── FogPipeline.h / .cpp       (Phase 5)
│   ├── gameplay/
│   │   ├── SplineUtil.h / .cpp        (Phase 4)
│   │   ├── LaneFollower.h / .cpp
│   │   ├── SpawnSystem.h / .cpp
│   │   └── FogSystem.h / .cpp         (Phase 5)
│   └── main.cpp
├── tests/
│   ├── test_mirror.cpp
│   ├── test_maploader.cpp
│   ├── test_spline.cpp
│   └── test_spawn.cpp
└── CMakeLists.txt
```