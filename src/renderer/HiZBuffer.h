#pragma once
#include "renderer/Device.h"
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>
#include <vector>

namespace glory {

/// Hierarchical Z-buffer (Hi-Z) for GPU occlusion culling.
/// Generated from the scene depth buffer each frame via 2×2 max-reduction mips.
/// Used by hiz_cull.comp to reject draw calls whose AABB depth is occluded.
class HiZBuffer {
public:
    void init(Device& device, uint32_t width, uint32_t height);
    /// Downsample depthView into the Hi-Z mip pyramid.
    void generate(VkCommandBuffer cmd, VkImageView depthView, uint32_t frameIdx);
    VkImageView getMipView(uint32_t mip) const;
    uint32_t    getMipCount() const { return m_mipCount; }
    void        destroy();

private:
    VkDevice      m_device     = VK_NULL_HANDLE;
    VmaAllocator  m_allocator  = VK_NULL_HANDLE;
    VkImage       m_hizImage   = VK_NULL_HANDLE;
    VmaAllocation m_hizAlloc   = VK_NULL_HANDLE;
    uint32_t      m_mipCount   = 0;
    std::vector<VkImageView> m_mipViews;
    VkPipeline       m_genPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_genLayout   = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descSets;
};

} // namespace glory
