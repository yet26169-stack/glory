#pragma once

#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace glory {

class Device;

// ── ExplosionRenderer ──────────────────────────────────────────────────────────
// Renders triggered explosion effects in two additive passes:
//   Pass 1 — Shockwave disk   (expanding ground ring, orange/white → scorch)
//   Pass 2 — Fireball sphere  (rising FBM fire sphere, additive)
//
// No descriptor sets — everything via a 112-byte push-constant block.
// Supports up to MAX_EXPLOSIONS simultaneous active explosions.
// Pattern mirrors ConeAbilityRenderer.
class ExplosionRenderer {
public:
    ExplosionRenderer()  = default;
    ~ExplosionRenderer() { destroy(); }

    void init(const Device& device, VkRenderPass renderPass);

    // Trigger a new explosion at the given world position.
    void addExplosion(glm::vec3 center);

    // Advance all active explosion timers. Call once per frame.
    void update(float dt);

    // Record draw commands inside the active render pass.
    void render(VkCommandBuffer  cmd,
                const glm::mat4& viewProj,
                const glm::vec3& cameraPos,
                float            appTime);

    void destroy();

    static constexpr float DURATION       = 2.2f;  // rings + spikes visible for ~2s
    static constexpr int   MAX_EXPLOSIONS = 8;

private:
    // ── 112-byte push-constant block ──────────────────────────────────────────
    struct ExplosionPC {
        glm::mat4 viewProj;    // 64 B
        glm::vec3 center;      // 12 B
        float     elapsed;     //  4 B
        glm::vec3 cameraPos;   // 12 B
        float     maxRadius;   //  4 B
        float     alpha;       //  4 B
        float     appTime;     //  4 B
        float     pad[2];      //  8 B
    };                         // = 112 B
    static_assert(sizeof(ExplosionPC) == 112, "ExplosionPC must be exactly 112 bytes");

    struct ExplosionInstance {
        glm::vec3 center;
        float     elapsed = 0.0f;
        float     maxRadius = 4.0f;
    };

    // Disk vertex: packed polar (cos θ, 0, sin θ) + UV (ringFrac, thetaFrac)
    struct DiskVertex {
        glm::vec3 pos;
        glm::vec2 uv;
    };

    // ── Vulkan handles ────────────────────────────────────────────────────────
    VkDevice     m_dev       = VK_NULL_HANDLE;
    VmaAllocator m_allocator = VK_NULL_HANDLE;

    Buffer   m_diskVB;
    Buffer   m_diskIB;
    uint32_t m_diskIndexCount = 0;

    Buffer   m_sphereVB;
    Buffer   m_sphereIB;
    uint32_t m_sphereIndexCount = 0;

    VkPipelineLayout m_layout             = VK_NULL_HANDLE;
    VkPipeline       m_shockwavePipeline  = VK_NULL_HANDLE;
    VkPipeline       m_fireballPipeline   = VK_NULL_HANDLE;

    std::vector<ExplosionInstance> m_active;

    // ── Internal helpers ──────────────────────────────────────────────────────
    void generateDiskMesh(const Device& device);
    void generateSphereMesh(const Device& device);

    VkPipeline createPipeline(VkRenderPass        renderPass,
                              const std::string&  vertSpv,
                              const std::string&  fragSpv,
                              VkCullModeFlags     cullMode,
                              VkBlendFactor       srcFactor,
                              VkBlendFactor       dstFactor);

    static VkShaderModule loadShader(VkDevice dev, const std::string& path);
};

} // namespace glory
