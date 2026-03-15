#pragma once

#include "renderer/Buffer.h"
#include "renderer/Device.h"
#include "renderer/Image.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

namespace glory {

class FogOfWarRenderer {
public:
    void init(const Device& device);
    void updateVisibility(const uint8_t* grid, uint32_t width, uint32_t height);
    void dispatch(VkCommandBuffer cmd);
    VkImageView getVisibilityView() const { return m_hiResOutput.getImageView(); }
    VkSampler   getSampler()        const { return m_sampler; }
    void destroy();
    ~FogOfWarRenderer() { destroy(); }

private:
    const Device* m_device = nullptr;

    // Images
    Image  m_lowResInput;   // 128×128 R8_UNORM — CPU uploads here
    Image  m_hiResOutput;   // 512×512 R8_UNORM — compute result (fragment shader reads)
    Image  m_pingPong;      // 512×512 R8_UNORM — intermediate blur target

    Buffer m_stagingBuffer; // 128×128 bytes, host-visible
    VkSampler m_sampler = VK_NULL_HANDLE;

    bool m_dirty         = true;
    bool m_firstDispatch = true;  // tracks first-frame layout state

    // Shared descriptor layout: binding 0=sampler2D, binding 1=image2D(r8)
    VkDescriptorSetLayout m_descLayout   = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool     = VK_NULL_HANDLE;
    VkDescriptorSet       m_upsampleDesc = VK_NULL_HANDLE;  // lowRes → hiRes
    VkDescriptorSet       m_hBlurDesc    = VK_NULL_HANDLE;  // hiRes → pingPong
    VkDescriptorSet       m_vBlurDesc    = VK_NULL_HANDLE;  // pingPong → hiRes

    // Upsample pipeline
    VkPipelineLayout m_upsampleLayout   = VK_NULL_HANDLE;
    VkPipeline       m_upsamplePipeline = VK_NULL_HANDLE;

    // Blur pipeline (push constant int horizontal)
    VkPipelineLayout m_blurLayout   = VK_NULL_HANDLE;
    VkPipeline       m_blurPipeline = VK_NULL_HANDLE;

    void createSampler();
    void createImages();
    void transitionInitialLayouts();
    void uploadInitialGrid();
    void createDescriptors();
    void writeDescriptors();
    void createUpsamplePipeline();
    void createBlurPipeline();
    VkShaderModule loadShader(const std::string& path);

    // Helper: record an image layout transition into cmd
    static void imageBarrier(VkCommandBuffer cmd, VkImage image,
                              VkImageLayout oldLayout, VkImageLayout newLayout,
                              VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess,
                              VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage);
};

} // namespace glory
