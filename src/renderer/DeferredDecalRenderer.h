#pragma once

#include "renderer/Device.h"
#include "renderer/Buffer.h"
#include "renderer/RenderFormats.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace glory {

class BindlessDescriptors;

struct DeferredDecal {
    glm::mat4 transform;        // world-space transform of the decal box
    glm::vec4 color = {1,1,1,1};
    float     opacity = 1.0f;
    float     lifetime = -1.0f;  // remaining seconds (-1 = permanent)
    float     fadeTime = 0.5f;   // seconds to fade out before removal
    float     fadeDistance = 0.1f; // box-edge fade width in local space
    uint32_t  textureIndex = 0;  // bindless texture index for decal albedo
};

class DeferredDecalRenderer {
public:
    static constexpr uint32_t MAX_DECALS = 128;

    DeferredDecalRenderer(const Device& device, const RenderFormats& formats,
                          BindlessDescriptors& bindless);
    ~DeferredDecalRenderer();

    DeferredDecalRenderer(const DeferredDecalRenderer&) = delete;
    DeferredDecalRenderer& operator=(const DeferredDecalRenderer&) = delete;

    /// Add a deferred decal. Ring-buffer: oldest replaced when full.
    void addDecal(const DeferredDecal& decal);

    /// Tick lifetimes, fade opacity, remove expired decals.
    void update(float dt);

    /// Render all active decals. Must be called within a dynamic rendering scope
    /// with depth read-only and color LOAD.
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                VkImageView depthView, VkSampler depthSampler,
                const glm::vec2& screenSize, float nearPlane, float farPlane);

    /// Recreate depth descriptor when framebuffer is resized.
    void updateDepthDescriptor(VkImageView depthView, VkSampler depthSampler);

    void destroyAll();

private:
    struct DecalUBO {
        glm::mat4 viewProj;
        glm::vec2 screenSize;
        float     nearPlane;
        float     farPlane;
    };

    struct DecalPushConstants {
        glm::mat4 invDecalModel;  //  0-63
        glm::vec4 decalColor;     // 64-79
        float     opacity;        // 80-83
        float     fadeDistance;    // 84-87
        uint32_t  texIdx;         // 88-91
        float     _pad;           // 92-95
    };
    static_assert(sizeof(DecalPushConstants) == 96, "DecalPushConstants must be 96 bytes");

    const Device&       m_device;
    BindlessDescriptors& m_bindless;

    VkDescriptorSetLayout m_descLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool     = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet      = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline     = VK_NULL_HANDLE;

    Buffer m_cubeVBO;
    Buffer m_cubeIBO;
    Buffer m_ubo;
    void*  m_uboMapped = nullptr;

    uint32_t m_cubeIndexCount = 0;

    struct ActiveDecal {
        DeferredDecal decal;
        float         elapsed = 0.0f;
        float         currentOpacity;
    };
    std::vector<ActiveDecal> m_decals;

    void createCubeMesh();
    void createPipeline(const RenderFormats& formats);
    void createDescriptors(VkImageView depthView, VkSampler depthSampler);
};

} // namespace glory
