#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>

namespace glory {

class Device;
class Buffer;

// Global bindless descriptor set (set 1 in the pipeline layout).
// Holds large, sparsely-bound arrays that any shader can index into
// with nonuniformEXT.  A single VkDescriptorSet is shared across all
// frames-in-flight because UPDATE_AFTER_BIND allows concurrent writes.
class BindlessDescriptors {
public:
    static constexpr uint32_t MAX_TEXTURES        = 4096;
    static constexpr uint32_t MAX_STORAGE_BUFFERS  = 256;

    explicit BindlessDescriptors(const Device& device);
    ~BindlessDescriptors();

    BindlessDescriptors(const BindlessDescriptors&)            = delete;
    BindlessDescriptors& operator=(const BindlessDescriptors&) = delete;

    void cleanup();

    // Thread-safe — allocates the next free texture slot, writes the
    // descriptor, and returns the bindless index for use in shaders.
    uint32_t registerTexture(VkImageView view, VkSampler sampler);

    // Thread-safe — allocates the next free SSBO slot and returns its index.
    uint32_t registerStorageBuffer(VkBuffer buffer,
                                   VkDeviceSize offset,
                                   VkDeviceSize range);

    VkDescriptorSetLayout getLayout()  const { return m_layout; }
    VkDescriptorSet       getSet()     const { return m_set; }

private:
    const Device& m_device;

    VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool      m_pool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_set    = VK_NULL_HANDLE;

    std::mutex m_mutex;
    uint32_t   m_nextTextureSlot  = 0;
    uint32_t   m_nextStorageSlot  = 0;

    bool m_cleaned = false;

    void createLayout();
    void createPool();
    void createSet();
};

} // namespace glory
