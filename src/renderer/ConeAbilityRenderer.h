#pragma once

#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace glory {

class Device;

// ── ConeAbilityRenderer ────────────────────────────────────────────────────────
// Renders the W-ability cone effect in three additive passes:
//   Pass 1 — Interior energy/smoke   (animated FBM noise, green→purple)
//   Pass 2 — Surface wireframe grid  (scrolling retro-grid, neon green)
//   Pass 3 — Lightning arcs          (CPU-jittered line strips, yellow/white)
//
// No descriptor sets — everything via a 128-byte push-constant block.
// Pattern mirrors ShieldBubbleRenderer.
class ConeAbilityRenderer {
public:
    ConeAbilityRenderer()  = default;
    ~ConeAbilityRenderer() { destroy(); }

    // Call once during engine init.
    void init(const Device& device, VkRenderPass renderPass);

    // Tick lightning jitter (~20 Hz internally). Call each frame while active.
    void update(float dt,
                const glm::vec3& apex,
                const glm::vec3& axisDir,
                float halfAngleRad,
                float range,
                float elapsed);   // seconds since W was cast (for wave-front lightning)

    // Record draw commands inside the active render pass.
    void render(VkCommandBuffer  cmd,
                const glm::mat4& viewProj,
                const glm::vec3& apex,
                const glm::vec3& axisDir,
                float            halfAngleRad,
                float            range,
                const glm::vec3& cameraPos,
                float            time,
                float            elapsed,
                float            alpha);

    void destroy();

private:
    // ── 128-byte push-constant block ──────────────────────────────────────────
    struct ConePC {
        glm::mat4 viewProj;      // 64 B
        glm::vec3 apex;          // 12 B
        float     time;          //  4 B
        glm::vec3 axisDir;       // 12 B
        float     halfAngleTan;  //  4 B
        glm::vec3 cameraPos;     // 12 B
        float     range;         //  4 B
        float     alpha;         //  4 B
        float     elapsed;       //  4 B  ← seconds since W was cast
        float     phase;         //  4 B  ← per-bolt phase for flicker
        float     pad[1];        //  4 B
    };                           // = 128 B total
    static_assert(sizeof(ConePC) == 128, "ConePC must be exactly 128 bytes");

    // ── Cone vertex: compact encoding for vertex shader decode ───────────────
    struct ConeVertex {
        glm::vec3 pos;  // (cos θ, sin θ, ring_fraction v∈[0,1])
        glm::vec2 uv;   // (segment_fraction, ring_fraction)
    };

    // ── Lightning constants ───────────────────────────────────────────────────
    static constexpr int   N_BOLTS         = 3;
    static constexpr int   N_BOLT_SEGS     = 14;
    static constexpr float LIGHTNING_RATE  = 0.05f;  // regenerate every 50 ms

    // ── Vulkan handles ────────────────────────────────────────────────────────
    VkDevice         m_dev       = VK_NULL_HANDLE;
    VmaAllocator     m_allocator = VK_NULL_HANDLE;

    Buffer   m_coneVB;
    Buffer   m_coneIB;
    uint32_t m_coneIndexCount = 0;

    Buffer   m_lightningVB;
    float    m_lightningTimer = 0.0f;

    VkPipelineLayout m_layout            = VK_NULL_HANDLE;
    VkPipeline       m_energyPipeline    = VK_NULL_HANDLE;
    VkPipeline       m_gridPipeline      = VK_NULL_HANDLE;
    VkPipeline       m_lightningPipeline = VK_NULL_HANDLE;

    // ── RNG for lightning ─────────────────────────────────────────────────────
    std::mt19937                          m_rng{1337u};
    std::uniform_real_distribution<float> m_rdist{0.0f, 1.0f};

    // ── Internal helpers ──────────────────────────────────────────────────────
    void generateConeMesh(const Device& device);
    void createLightningBuffer();
    void regenerateLightning(const glm::vec3& apex,
                             const glm::vec3& axisDir,
                             float halfAngleRad,
                             float range);

    // Generic pipeline factory.
    VkPipeline createPipeline(VkRenderPass        renderPass,
                              const std::string&  vertSpv,
                              const std::string&  fragSpv,
                              VkPrimitiveTopology topology,
                              VkCullModeFlags     cullMode,
                              VkBlendFactor       srcFactor,
                              VkBlendFactor       dstFactor);

    static VkShaderModule loadShader(VkDevice dev, const std::string& path);
};

} // namespace glory
