#pragma once

#include "renderer/Image.h"
#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

namespace glory {

class Device;

class ShadowMap {
public:
    static constexpr uint32_t SHADOW_MAP_SIZE = 2048;

    ShadowMap(const Device& device);
    ~ShadowMap();

    ShadowMap(const ShadowMap&)            = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    void cleanup();

    VkRenderPass     getRenderPass()     const { return m_renderPass; }
    VkFramebuffer    getFramebuffer()    const { return m_framebuffer; }
    VkPipeline       getPipeline()       const { return m_pipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }
    VkImageView      getDepthView()      const { return m_depthImage.getImageView(); }
    VkImage          getDepthImage()     const { return m_depthImage.getImage(); }
    VkSampler        getSampler()        const { return m_sampler; }
    VkDescriptorSet  getDescSet()        const { return m_descSet; }

    void updateLightMatrix(const glm::mat4& lightVP);

private:
    const Device& m_device;

    Image m_depthImage;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkRenderPass     m_renderPass     = VK_NULL_HANDLE;
    VkFramebuffer    m_framebuffer    = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet    = VK_NULL_HANDLE;

    // Light-space matrix UBO (persistently mapped via Buffer)
    Buffer m_lightMatBuffer;

    bool m_cleaned = false;

    void createDepthResources();
    void createRenderPass();
    void createFramebuffer();
    void createDescriptors();
    void createPipeline();
    void createSampler();
};

} // namespace glory
