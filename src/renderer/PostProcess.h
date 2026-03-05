#pragma once

#include "renderer/Image.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace glory {

class Device;
class Swapchain;

struct PostProcessParams {
    float exposure         = 1.0f;
    float gamma            = 2.2f;
    float bloomIntensity   = 0.3f;
    float bloomThreshold   = 1.0f;
    float vignetteStrength = 0.4f;
    float vignetteRadius   = 0.75f;
    float chromaStrength   = 0.0f;
    float filmGrain        = 0.03f;
    float toneMapMode      = 0.0f; // 0=ACES, 1=Reinhard, 2=Uncharted2
    float fxaaEnabled      = 1.0f; // >0.5 = on
    float sharpenStrength  = 0.0f;
    float dofStrength      = 0.0f; // 0 = off
    float dofFocusDist     = 5.0f;
    float dofRange         = 3.0f;
    float saturation       = 1.0f; // 1 = neutral
    float colorTemp        = 0.0f; // -1 cool .. +1 warm
    float outlineStrength  = 0.0f; // 0 = off, 1 = full outline
    float outlineThreshold = 0.1f; // depth-edge sensitivity
    float godRaysStrength  = 0.0f; // 0 = off
    float godRaysDecay     = 0.97f;
    float lightScreenX     = 0.5f; // light position in screen UV
    float lightScreenY     = 0.5f;
    float godRaysDensity   = 0.5f;
    float godRaysSamples   = 64.0f;
    float autoExposure     = 0.0f; // >0.5 = auto-exposure enabled
    float heatDistortion   = 0.0f; // 0 = off, strength of heat shimmer
    float ditheringStrength = 0.0f; // 0 = off, subtle banding reduction
    float pad3             = 0.0f;
};

class PostProcess {
public:
    PostProcess(const Device& device, const Swapchain& swapchain);
    ~PostProcess();

    PostProcess(const PostProcess&)            = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    void cleanup();
    void recreate(const Swapchain& swapchain);

    VkRenderPass     getRenderPass()     const { return m_renderPass; }
    VkPipeline       getPipeline()       const { return m_pipeline; }
    VkPipeline       getMobaPipeline()   const { return m_mobaPipeline; }
    VkPipelineLayout getPipelineLayout() const { return m_pipelineLayout; }
    VkDescriptorSet  getDescSet()        const { return m_descSet; }
    const std::vector<VkFramebuffer>& getFramebuffers() const { return m_framebuffers; }

    // The HDR target the scene should render into
    VkRenderPass  getHDRRenderPass() const { return m_hdrRenderPass; }
    VkFramebuffer getHDRFramebuffer() const { return m_hdrFramebuffer; }
    VkImageView   getHDRImageView()  const { return m_hdrImage.getImageView(); }
    VkImageView   getHDRDepthView()  const { return m_hdrDepthImage.getImageView(); }
    VkImage       getHDRDepthImage() const { return m_hdrDepthImage.getImage(); }

    PostProcessParams& getParams() { return m_params; }

    VkImageView getDummyBloomImageView() const { return m_dummyBloomImage.getImageView(); }
    VkImageView getDummySSAOImageView()  const { return m_dummySSAOImage.getImageView(); }

    void updateBloomDescriptor(VkImageView bloomView);
    void updateSSAODescriptor(VkImageView ssaoView);
    void updateDepthDescriptor(VkImageView depthView);

private:
    const Device& m_device;

    PostProcessParams m_params;

    // HDR offscreen render target
    Image            m_hdrImage;
    Image            m_hdrDepthImage;
    VkRenderPass     m_hdrRenderPass  = VK_NULL_HANDLE;
    VkFramebuffer    m_hdrFramebuffer = VK_NULL_HANDLE;

    // 1×1 white dummy images used for SSAO/Bloom descriptors in MOBA mode
    // (valid bound image, but costs essentially no memory/bandwidth)
    Image            m_dummyBloomImage;
    Image            m_dummySSAOImage;

    // Post-process output pass (to swapchain)
    VkRenderPass     m_renderPass     = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;  // full-quality (all effects)
    VkPipeline       m_mobaPipeline   = VK_NULL_HANDLE;  // MOBA-mode (tone map + FXAA only)
    std::vector<VkFramebuffer> m_framebuffers;

    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet    = VK_NULL_HANDLE;
    VkSampler             m_sampler    = VK_NULL_HANDLE;

    bool m_cleaned = false;

    void createHDRResources(const Swapchain& swapchain);
    void createHDRRenderPass(const Swapchain& swapchain);
    void createHDRFramebuffer(const Swapchain& swapchain);
    void createOutputRenderPass(const Swapchain& swapchain);
    void createOutputFramebuffers(const Swapchain& swapchain);
    void createSampler();
    void createDescriptors();
    void updateDescriptors();
    void createPipeline(const Swapchain& swapchain);

    void destroySwapchainResources();
};

} // namespace glory
