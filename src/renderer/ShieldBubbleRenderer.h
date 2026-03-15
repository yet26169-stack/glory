#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstdint>

#include "renderer/Buffer.h"
#include "renderer/RenderFormats.h"

namespace glory {

class Device;

// Renders a two-pass glass-sphere shield effect around a world-space point.
//
// Pass 1 — back-faces, standard alpha blend: soft inner tint of the sphere.
// Pass 2 — front-faces, additive blend:      bright Fresnel rim that glows.
//
// No descriptor sets required — everything is delivered via push constants.
class ShieldBubbleRenderer {
public:
    ShieldBubbleRenderer() = default;
    ~ShieldBubbleRenderer() { destroy(); }

    void init(const Device& device, const RenderFormats& formats);

    // Call once per frame, inside the active render pass, after all opaque draws.
    // alpha  [0,1] — master opacity; fade-in/out is the caller's responsibility.
    // time   — application time in seconds (drives the subtle pulse animation).
    void render(VkCommandBuffer cmd,
                const glm::mat4& viewProj,
                const glm::vec3& sphereCenter,
                const glm::vec3& cameraPos,
                float radius,
                float time,
                float alpha);

    void destroy();

private:
    // 112-byte push-constant block shared by both passes.
    struct ShieldPC {
        glm::mat4 viewProj;       // 64 B
        glm::vec3 sphereCenter;   // 12 B
        float     radius;         //  4 B
        glm::vec3 cameraPos;      // 12 B
        float     time;           //  4 B
        float     alpha;          //  4 B
        float     _pad[3];        // 12 B
    };                            // total: 112 B  (<= 128 B Vulkan minimum)

    VkDevice m_dev = VK_NULL_HANDLE;

    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    uint32_t m_indexCount = 0;

    VkPipelineLayout m_pipelineLayout    = VK_NULL_HANDLE;
    VkPipeline       m_backfacePipeline  = VK_NULL_HANDLE;  // pass 1: inner tint
    VkPipeline       m_frontfacePipeline = VK_NULL_HANDLE;  // pass 2: Fresnel glow

    void       generateSphere(const Device& device);
    VkPipeline createPipeline(const RenderFormats& formats,
                              VkCullModeFlags cullMode,
                              VkBlendFactor   srcColorFactor,
                              VkBlendFactor   dstColorFactor);

    static VkShaderModule loadShaderModule(VkDevice dev, const char* spirvPath);
};

} // namespace glory
