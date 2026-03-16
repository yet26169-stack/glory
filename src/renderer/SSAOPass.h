#pragma once

#include "renderer/Device.h"
#include "renderer/Image.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace glory {

class SSAOPass {
public:
    SSAOPass() = default;

    void init(const Device& device, uint32_t width, uint32_t height,
              VkImageView depthView, VkSampler depthSampler);

    void dispatch(VkCommandBuffer cmd, const glm::mat4& invProj,
                  uint32_t sampleCount = 16, float radius = 0.5f,
                  float bias = 0.025f, float intensity = 1.5f);

    void recreate(uint32_t width, uint32_t height,
                  VkImageView depthView, VkSampler depthSampler);

    void destroy();

    VkImageView getAOView()    const { return m_blurPingPong[1].getImageView(); }
    VkImage     getAOImage()   const { return m_blurPingPong[1].getImage(); }
    VkSampler   getAOSampler() const { return m_aoSampler; }
    uint32_t    aoWidth()      const { return m_aoWidth; }
    uint32_t    aoHeight()     const { return m_aoHeight; }

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool e) { m_enabled = e; }

private:
    const Device* m_device = nullptr;
    uint32_t m_aoWidth  = 0;
    uint32_t m_aoHeight = 0;
    bool m_enabled = true;

    // AO output (half-res R8)
    Image m_aoImage;
    // Ping-pong blur images (half-res R8)
    std::array<Image, 2> m_blurPingPong;

    VkSampler m_aoSampler = VK_NULL_HANDLE;

    // Hemisphere sample kernel
    std::array<glm::vec4, 32> m_kernel;

    // SSAO compute
    VkDescriptorSetLayout m_ssaoDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_ssaoDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_ssaoDescSet    = VK_NULL_HANDLE;
    VkPipelineLayout      m_ssaoPipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_ssaoPipeline   = VK_NULL_HANDLE;

    // Blur compute
    VkDescriptorSetLayout m_blurDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_blurDescPool   = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> m_blurDescSets{}; // [0]=H, [1]=V
    VkPipelineLayout      m_blurPipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_blurPipeline   = VK_NULL_HANDLE;

    void generateKernel();
    void createImages();
    void createSampler();
    void createSSAOPipeline();
    void createSSAODescriptors(VkImageView depthView, VkSampler depthSampler);
    void createBlurPipeline();
    void createBlurDescriptors(VkImageView depthView, VkSampler depthSampler);
    void destroyResources();
};

} // namespace glory
