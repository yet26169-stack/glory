#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <vector>

namespace glory {

/// Two-frame latency, VkFence-gated CPU readback of the fogMap image.
/// Used by: MinionSystem (vision queries without GPU sync), HUD minimap.
class FogReadback {
public:
    void init(VkDevice device, VkPhysicalDevice physDevice,
              uint32_t mapW, uint32_t mapH, uint32_t framesInFlight);

    /// Issue a GPU→CPU copy of fogImage at the end of the compute pass.
    void requestReadback(VkCommandBuffer cmd, VkImage fogImage, uint32_t frameIdx);

    /// Returns a pointer to the latest available readback (nullptr if not ready).
    const uint8_t* getReadback(uint32_t frameIdx) const;

    void destroy();

private:
    struct Frame {
        VkBuffer      buf     = VK_NULL_HANDLE;
        VmaAllocation alloc   = VK_NULL_HANDLE;
        void*         mapped  = nullptr;
        VkFence       fence   = VK_NULL_HANDLE;
        bool          pending = false;
    };
    std::vector<Frame> m_frames;
    VkDevice           m_device  = VK_NULL_HANDLE;
    VmaAllocator       m_vma     = VK_NULL_HANDLE;
    uint32_t           m_w = 0, m_h = 0;
};

} // namespace glory
