#pragma once

#include "renderer/Device.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>

namespace glory {

class InkingPass {
public:
    void init(const Device& device, VkRenderPass renderPass,
              VkImageView characterDepthView, VkSampler sampler);

    ~InkingPass() { destroy(); }

    void render(VkCommandBuffer cmd, float threshold, float thickness,
                const glm::vec4& inkColor);

    void updateInput(VkImageView characterDepthView);

    void destroy();

private:
    const Device* m_device = nullptr;

    VkDescriptorSetLayout m_descLayout     = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool       = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet        = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline       = VK_NULL_HANDLE;
    VkSampler             m_sampler        = VK_NULL_HANDLE;

    struct InkPC {
        glm::vec4 inkColor;
        float     threshold;
        float     thickness;
    };

    void createDescriptorSet(VkImageView characterDepthView);
    void createPipeline(VkRenderPass renderPass);
    VkShaderModule createShaderModule(const std::vector<char>& code);
};

} // namespace glory
