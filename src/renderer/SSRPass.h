#pragma once

#include "renderer/Device.h"
#include "renderer/Image.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <cstdint>

namespace glory {

// Screen-space reflections compute pass.
// Reads scene color + depth, writes half-res reflection color + confidence.
// Output is sampled by the water shader for real-time reflections.
class SSRPass {
public:
    SSRPass() = default;

    void init(const Device& device, uint32_t width, uint32_t height,
              VkImageView colorView, VkImageView depthView,
              VkSampler sceneSampler);

    void dispatch(VkCommandBuffer cmd,
                  const glm::mat4& viewProj,
                  const glm::mat4& invViewProj,
                  const glm::vec3& cameraPos,
                  float maxDistance = 50.0f,
                  float thickness  = 0.3f,
                  uint32_t maxSteps = 64,
                  float stepStride = 2.0f);

    void recreate(uint32_t width, uint32_t height,
                  VkImageView colorView, VkImageView depthView,
                  VkSampler sceneSampler);

    void destroy();

    VkImageView getReflectionView()    const { return m_ssrImage.getImageView(); }
    VkImage     getReflectionImage()   const { return m_ssrImage.getImage(); }
    VkSampler   getReflectionSampler() const { return m_ssrSampler; }
    uint32_t    ssrWidth()  const { return m_ssrWidth; }
    uint32_t    ssrHeight() const { return m_ssrHeight; }

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool e) { m_enabled = e; }

private:
    const Device* m_device = nullptr;
    uint32_t m_ssrWidth  = 0;
    uint32_t m_ssrHeight = 0;
    uint32_t m_fullWidth = 0;
    uint32_t m_fullHeight = 0;
    bool m_enabled = true;

    // Half-res RGBA16F reflection output
    Image m_ssrImage;

    VkSampler m_ssrSampler = VK_NULL_HANDLE;

    // Compute pipeline
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet    = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline   = VK_NULL_HANDLE;

    void createImage();
    void createSampler();
    void createPipeline();
    void createDescriptors(VkImageView colorView, VkImageView depthView,
                           VkSampler sceneSampler);
    void destroyResources();
};

} // namespace glory
