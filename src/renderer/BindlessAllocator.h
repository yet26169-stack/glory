#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace glory {

/// Bindless resource allocator backed by VK_EXT_descriptor_indexing (core in Vulkan 1.2).
/// Eliminates per-draw descriptor set updates by indexing all textures and
/// buffers through a single large UPDATE_AFTER_BIND descriptor set.
///
/// Enable in Device.cpp:
///   VkPhysicalDeviceDescriptorIndexingFeatures indexing{};
///   indexing.descriptorBindingPartiallyBound             = VK_TRUE;
///   indexing.runtimeDescriptorArray                      = VK_TRUE;
///   indexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
///   indexing.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
class BindlessAllocator {
public:
    static constexpr uint32_t MAX_TEXTURES = 4096;
    static constexpr uint32_t MAX_BUFFERS  = 1024;

    void init(VkDevice device, uint32_t framesInFlight);

    /// Register a texture; returns its bindless index (push-constant to shader).
    uint32_t allocTexture(VkImageView view, VkSampler sampler);
    /// Register a buffer range; returns its bindless index.
    uint32_t allocBuffer(VkBuffer buf, VkDeviceSize offset, VkDeviceSize range);

    void freeTexture(uint32_t idx);
    void freeBuffer (uint32_t idx);

    VkDescriptorSetLayout getLayout() const { return m_layout; }
    VkDescriptorSet       getSet()    const { return m_set; }
    void                  destroy();

private:
    VkDevice              m_device = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool   = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorSet       m_set    = VK_NULL_HANDLE;

    std::vector<uint32_t> m_freeTexSlots;
    std::vector<uint32_t> m_freeBufSlots;
};

} // namespace glory
