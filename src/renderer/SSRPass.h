#pragma once

#include "renderer/Device.h"
#include "renderer/Image.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>

namespace glory {

class SSRPass {
public:
    SSRPass() = default;

    void init(const Device& device, uint32_t width, uint32_t height,
              VkImageView depthView, VkSampler depthSampler,
              VkImageView hdrColorView, VkSampler hdrSampler,
              VkImageView hizView, VkSampler hizSampler);

    void dispatch(VkCommandBuffer cmd,
                  const glm::mat4& invProj, const glm::mat4& proj,
                  const glm::mat4& view,
                  float maxDistance = 50.0f, float thickness = 0.3f,
                  uint32_t maxSteps = 64);

    void recreate(uint32_t width, uint32_t height,
                  VkImageView depthView, VkSampler depthSampler,
                  VkImageView hdrColorView, VkSampler hdrSampler,
                  VkImageView hizView, VkSampler hizSampler);

    void destroy();

    VkImageView getReflectionView()    const { return m_reflectionImage.getImageView(); }
    VkImage     getReflectionImage()   const { return m_reflectionImage.getImage(); }
    VkSampler   getReflectionSampler() const { return m_sampler; }
    uint32_t    reflWidth()            const { return m_width; }
    uint32_t    reflHeight()           const { return m_height; }

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool e) { m_enabled = e; }

private:
    const Device* m_device = nullptr;
    uint32_t m_width  = 0;
    uint32_t m_height = 0;
    bool m_enabled = false; // off by default (HIGH_QUALITY+)

    Image m_reflectionImage; // half-res RGBA16F

    VkSampler m_sampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet    = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline   = VK_NULL_HANDLE;

    void createImage();
    void createSampler();
    void createPipeline();
    void createDescriptors(VkImageView depthView, VkSampler depthSampler,
                           VkImageView hdrColorView, VkSampler hdrSampler,
                           VkImageView hizView, VkSampler hizSampler);
    void destroyResources();
};

} // namespace glory
