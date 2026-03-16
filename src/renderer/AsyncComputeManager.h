#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>

namespace glory {

class Device;

// Manages an async compute queue with its own command buffers and
// a timeline semaphore for compute→graphics synchronization.
class AsyncComputeManager {
public:
    static constexpr uint32_t MAX_FRAMES = 2;

    AsyncComputeManager() = default;
    void init(const Device& device);
    void destroy();

    // Begin recording compute commands for this frame.
    VkCommandBuffer begin(uint32_t frameIndex);

    // End recording and submit to the compute queue.
    // Returns the timeline value the compute queue will signal.
    uint64_t submit(uint32_t frameIndex);

    // Wait on CPU until compute work for the given frame slot completes.
    void waitForCompute(uint32_t frameIndex);

    VkSemaphore getTimelineSemaphore() const { return m_timeline; }
    uint64_t    getLastSignalValue(uint32_t frameIndex) const { return m_frameValues[frameIndex]; }

    uint32_t    getQueueFamilyIndex() const { return m_queueFamilyIndex; }

private:
    const Device*   m_device           = nullptr;
    VkQueue         m_computeQueue     = VK_NULL_HANDLE;
    VkCommandPool   m_commandPool      = VK_NULL_HANDLE;
    uint32_t        m_queueFamilyIndex = 0;

    std::array<VkCommandBuffer, MAX_FRAMES> m_cmdBuffers{};

    // Timeline semaphore for compute→graphics sync
    VkSemaphore m_timeline      = VK_NULL_HANDLE;
    uint64_t    m_timelineCounter = 0;
    std::array<uint64_t, MAX_FRAMES> m_frameValues{};
};

} // namespace glory
