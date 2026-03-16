#pragma once

// GPU-accelerated spatial hashing and broad-phase collision detection.
// Replaces O(N²) CPU entity scans with GPU compute:
//   1. Upload entity positions + radii to GPU SSBO
//   2. spatial_hash.comp builds a hash grid
//   3. collision_broadphase.comp queries neighbors and outputs collision pairs
//   4. Read back compact pair list to CPU for game logic consumption
//
// Uses previous frame's results (1 frame latency) to avoid GPU↔CPU stalls.

#include "renderer/Buffer.h"
#include "renderer/Device.h"

#include <entt.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace glory {

// GPU-side entity representation (matches GLSL struct)
struct GpuEntity {
    float posX, posY, posZ;
    float radius;
    uint32_t entityId;      // entt integral ID
    uint32_t team;          // 0=player, 1=enemy, 2=neutral
    uint32_t flags;         // bit 0: projectile, bit 1: has stats (damageable)
    uint32_t _pad{0};
};
static_assert(sizeof(GpuEntity) == 32, "GpuEntity must be 32 bytes for GPU alignment");

// GPU-side collision pair result (matches GLSL struct)
struct GpuCollisionPair {
    uint32_t entityA;       // index into the GpuEntity array
    uint32_t entityB;       // index into the GpuEntity array
    float    distSq;        // squared XZ distance
    uint32_t _pad{0};
};
static_assert(sizeof(GpuCollisionPair) == 16, "GpuCollisionPair must be 16 bytes");

// CPU-side collision result with resolved ECS entity handles
struct CollisionResult {
    entt::entity entityA;
    entt::entity entityB;
    float        distSq;
};

// Push constants shared by both spatial_hash.comp and collision_broadphase.comp
struct SpatialHashPC {
    uint32_t entityCount;
    float    cellSize;
    float    invCellSize;
    uint32_t tableSize;
    uint32_t maxPerCell;
    uint32_t maxPairs;
};

class GpuCollisionSystem {
public:
    static constexpr uint32_t MAX_FRAMES   = 2;
    static constexpr uint32_t TABLE_SIZE   = 1024;
    static constexpr uint32_t MAX_PER_CELL = 16;
    static constexpr uint32_t MAX_ENTITIES = 2048;
    static constexpr uint32_t MAX_PAIRS    = 4096;
    static constexpr float    CELL_SIZE    = 6.0f; // world units per cell

    GpuCollisionSystem() = default;

    void init(const Device& device);
    void destroy();

    // Gather entity data from the ECS registry and upload to GPU staging buffer.
    // Call before dispatching compute.
    void uploadEntities(entt::registry& reg, uint32_t frameIndex);

    // Record spatial hash + broadphase dispatches into a compute command buffer.
    // Called between asyncCompute.begin() and asyncCompute.submit().
    void dispatch(VkCommandBuffer computeCmd, uint32_t frameIndex,
                  uint32_t computeFamily, uint32_t graphicsFamily);

    // Read back collision pairs from the PREVIOUS frame.
    // Call at the start of the frame before simulation tick.
    void readResults(uint32_t frameIndex);

    // ── Query API (operates on cached CPU results) ────────────────────────

    // Find the nearest enemy entity to `attacker` within `range`.
    // Returns entt::null if none found.
    entt::entity findNearestEnemy(entt::registry& reg,
                                  entt::entity attacker,
                                  float range) const;

    // Get all collision pairs involving projectiles hitting non-friendly entities.
    // Returns pairs where one entity has the projectile flag and the other has stats.
    struct ProjectileHit {
        entt::entity projectile;
        entt::entity target;
        float        distSq;
    };
    std::vector<ProjectileHit> getProjectileHits() const;

    // Get all raw collision results for this frame
    const std::vector<CollisionResult>& getCollisionResults() const { return m_results; }

    uint32_t getLastEntityCount() const { return m_lastEntityCount; }
    uint32_t getLastPairCount() const { return static_cast<uint32_t>(m_results.size()); }

private:
    const Device* m_device = nullptr;

    // Per-frame GPU buffers (double-buffered)
    struct FrameData {
        Buffer entityBuffer;    // GpuEntity SSBO (CPU_TO_GPU, persistent mapped)
        Buffer gridBuffer;      // Hash grid (GPU_ONLY)
        Buffer pairBuffer;      // Output pairs (GPU_ONLY, +4 bytes for atomic counter)
        Buffer pairReadback;    // CPU-readable copy of pair results

        GpuEntity* mappedEntities = nullptr;
        uint32_t   entityCount    = 0;

        VkDescriptorSet hashDescSet      = VK_NULL_HANDLE;
        VkDescriptorSet broadphaseDescSet = VK_NULL_HANDLE;
    };
    std::array<FrameData, MAX_FRAMES> m_frames{};

    // Pipelines
    VkPipeline       m_hashPipeline       = VK_NULL_HANDLE;
    VkPipeline       m_broadphasePipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_hashLayout         = VK_NULL_HANDLE;
    VkPipelineLayout m_broadphaseLayout   = VK_NULL_HANDLE;

    // Descriptors
    VkDescriptorSetLayout m_hashDescLayout       = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_broadphaseDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool             = VK_NULL_HANDLE;

    // CPU-side results
    std::vector<CollisionResult> m_results;

    // Entity index → entt::entity mapping for the current result set
    std::vector<entt::entity> m_entityMap;

    // GPU entity flags
    std::vector<uint32_t> m_entityFlags;

    uint32_t m_lastEntityCount = 0;

    void createPipelines();
    void createDescriptors();
    void createBuffers();
};

} // namespace glory
