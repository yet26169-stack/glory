#pragma once

#include "renderer/Device.h"
#include "renderer/RenderFormats.h"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>

namespace glory {

// Fullscreen radial/zoom blur post-process pass.
// When intensity > 0, blurs the scene radially from a screen-space center.
// Zero cost when inactive (render() is a no-op).
class RadialBlurPass {
public:
    void init(const Device& device, const RenderFormats& formats,
              VkImageView sceneView, VkSampler sampler);

    // Render the radial blur.  Skips entirely when intensity <= 0.
    // Must be called inside an active rendering pass targeting the output.
    void render(VkCommandBuffer cmd, glm::vec2 center, float intensity,
                float sampleCount = 12.0f, float falloffStart = 0.15f,
                float maxBlurDist = 0.08f);

    void updateDescriptorSet(VkImageView sceneView);
    void destroy();

private:
    const Device* m_device = nullptr;
    RenderFormats m_formats;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet    = VK_NULL_HANDLE;

    VkPipelineLayout m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline   = VK_NULL_HANDLE;

    struct PushConstants {
        glm::vec2 center;       //  8 bytes
        float     intensity;    //  4 bytes
        float     sampleCount;  //  4 bytes
        float     falloffStart; //  4 bytes
        float     maxBlurDist;  //  4 bytes
        float     _pad0;        //  4 bytes
        float     _pad1;        //  4 bytes
    }; // 32 bytes

    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDescriptorSet(VkImageView sceneView);
    void createPipeline();

    VkShaderModule createShaderModule(const std::vector<char>& code);
};

} // namespace glory
