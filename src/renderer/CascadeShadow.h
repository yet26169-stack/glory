#pragma once
// ── Cascade Shadow Maps (CSM) ─────────────────────────────────────────────
// 3 cascades, each in its own layer of a 2D image array.
// Cascade splits are determined per-frame from the camera near/far planes.
// Static geometry is rendered into these maps from the light's POV.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <array>
#include <vector>
#include "renderer/Buffer.h"

namespace glory {

class Device;

class CascadeShadow {
public:
    static constexpr uint32_t CASCADE_COUNT = 3;
    static constexpr uint32_t SHADOW_MAP_SIZE = 2048;

    CascadeShadow(const Device& device);
    ~CascadeShadow();

    CascadeShadow(const CascadeShadow&)            = delete;
    CascadeShadow& operator=(const CascadeShadow&) = delete;

    void init(const Device& device);
    void destroy();

    // Updates the cascade split distances and calculates light space matrices.
    std::array<glm::mat4, CASCADE_COUNT> computeCascadeVPs(
        const glm::mat4& view, float fov, float aspect, 
        float nearPlane, float farPlane, const glm::vec3& lightDir);

    void updateLightMatrices(const std::array<glm::mat4, CASCADE_COUNT>& vps);

    // Call at start of shadow pass
    void record(VkCommandBuffer cmd, uint32_t cascadeIndex);

    bool             isInitialised() const { return m_initialised; }
    VkRenderPass     getRenderPass() const { return m_renderPass; }
    VkFramebuffer    getFramebuffer(uint32_t i) const { return m_framebuffers[i]; }
    VkPipeline       getPipeline()   const { return m_pipeline; }
    VkDescriptorSet  getDescSet()    const { return m_descSet; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }
    VkImageView      getArrayView()  const { return m_arrayView; }
    VkSampler        getSampler()    const { return m_sampler; }

    static constexpr float CASCADE_SPLITS[CASCADE_COUNT] = { 0.1f, 0.3f, 1.0f };

private:
    const Device*    m_device = nullptr;
    bool             m_initialised = false;

    // GPU resources: 2D Image Array (3 layers)
    VkImage          m_depthImage     = VK_NULL_HANDLE;
    VmaAllocation    m_depthAlloc     = VK_NULL_HANDLE;
    VkImageView      m_arrayView      = VK_NULL_HANDLE; // all cascades
    VkImageView      m_layerViews[3]  = { VK_NULL_HANDLE }; // individual layers

    VkFramebuffer    m_framebuffers[3] = { VK_NULL_HANDLE };
    VkRenderPass     m_renderPass     = VK_NULL_HANDLE;
    VkSampler        m_sampler        = VK_NULL_HANDLE;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;

    // UBO: 3 × mat4 light-VP matrices (persistently mapped via Buffer)
    Buffer           m_uboBuffer;

    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet    = VK_NULL_HANDLE;

    void createDepthArray();
    void createRenderPass();
    void createFramebuffers();
    void createSampler();
    void createUBO();
    void createDescriptors();
    void createPipeline();
};

} // namespace glory
