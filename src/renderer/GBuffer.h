#pragma once

#include "renderer/Image.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace glory {

class Device;
class Swapchain;

class GBuffer {
public:
    GBuffer(const Device& device, const Swapchain& swapchain,
            VkDescriptorSetLayout geometryLayout);
    ~GBuffer();

    GBuffer(const GBuffer&)            = delete;
    GBuffer& operator=(const GBuffer&) = delete;

    void cleanup();
    void recreate(const Swapchain& swapchain);

    // G-buffer pass
    VkRenderPass    getGeometryPass()     const { return m_geometryPass; }
    VkFramebuffer   getGeometryFB()       const { return m_geometryFB; }
    VkPipeline      getGeometryPipeline() const { return m_geometryPipeline; }
    VkPipelineLayout getGeometryPipelineLayout() const { return m_geometryPipelineLayout; }

    // Lighting (composition) pass
    VkRenderPass    getLightingPass()     const { return m_lightingPass; }
    VkPipeline      getLightingPipeline() const { return m_lightingPipeline; }
    VkPipelineLayout getLightingPipelineLayout() const { return m_lightingPipelineLayout; }
    VkDescriptorSet  getLightingDescSet() const { return m_lightingDescSet; }
    const std::vector<VkFramebuffer>& getLightingFramebuffers() const { return m_lightingFBs; }

private:
    const Device& m_device;
    VkDescriptorSetLayout m_geometryLayout = VK_NULL_HANDLE;

    // G-buffer images
    Image m_albedoImage;   // RGBA8
    Image m_normalImage;   // RGBA16F
    Image m_positionImage; // RGBA16F
    Image m_depthImage;

    // G-buffer geometry pass
    VkRenderPass     m_geometryPass           = VK_NULL_HANDLE;
    VkFramebuffer    m_geometryFB             = VK_NULL_HANDLE;
    VkPipelineLayout m_geometryPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_geometryPipeline       = VK_NULL_HANDLE;

    // Lighting composition pass
    VkRenderPass     m_lightingPass           = VK_NULL_HANDLE;
    VkPipelineLayout m_lightingPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_lightingPipeline       = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_lightingFBs;

    // Lighting descriptor set (samples g-buffer)
    VkDescriptorSetLayout m_lightingDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_lightingDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_lightingDescSet    = VK_NULL_HANDLE;
    VkSampler             m_gbufferSampler     = VK_NULL_HANDLE;

    bool m_cleaned = false;

    void createGBufferImages(const Swapchain& swapchain);
    void createGeometryPass(const Swapchain& swapchain);
    void createGeometryFramebuffer(const Swapchain& swapchain);
    void createGeometryPipeline(const Swapchain& swapchain);
    void createLightingPass(const Swapchain& swapchain);
    void createLightingFramebuffers(const Swapchain& swapchain);
    void createLightingDescriptors();
    void createLightingPipeline(const Swapchain& swapchain);
    void createGBufferSampler();
    void updateLightingDescriptors();

    void destroySwapchainResources();
};

} // namespace glory
