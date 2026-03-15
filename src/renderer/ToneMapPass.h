#pragma once

#include "renderer/Device.h"

#include <vulkan/vulkan.h>
#include <vector>

namespace glory {

class ToneMapPass {
public:
    void init(const Device& device, VkRenderPass swapchainRenderPass,
              VkImageView hdrView, VkImageView bloomView, VkSampler sampler);

    void render(VkCommandBuffer cmd, float exposure, float bloomStrength,
                uint32_t enableVignette = 1, uint32_t enableColorGrade = 1);

    void updateDescriptorSets(VkImageView hdrView, VkImageView bloomView);
    void destroy();

private:
    const Device* m_device = nullptr;
    VkRenderPass m_swapchainRenderPass = VK_NULL_HANDLE;
    VkImageView m_hdrView = VK_NULL_HANDLE;
    VkImageView m_bloomView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    struct ToneMapPushConstants {
        float    exposure;
        float    bloomStrength;
        uint32_t enableVignette;    // 1=on, 0=off
        uint32_t enableColorGrade;  // 1=on, 0=off
    };

    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDescriptorSet();
    void createPipeline();

    VkShaderModule createShaderModule(const std::vector<char>& code);
};

} // namespace glory
