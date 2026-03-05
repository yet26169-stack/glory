#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace glory {

/// Manages submission to graphics, async compute, and transfer queues.
/// Coordinates timeline semaphore signalling so async compute (skinning, culling,
/// FoW visibility) overlaps with the previous frame's graphics work.
///
/// The existing Device already finds a TRANSFER-only queue; this class adds
/// async compute submission and VK_KHR_timeline_semaphore coordination.
class QueueManager {
public:
    void init(VkDevice device,
              uint32_t graphicsFamily, uint32_t computeFamily,
              uint32_t transferFamily, uint32_t framesInFlight);

    /// Submit work to the async compute queue; signal timelineSemaphore at value=tick.
    void submitCompute(VkCommandBuffer cmd, uint64_t signalValue);

    /// Submit to graphics queue; wait on compute semaphore before vertex stage.
    void submitGraphics(VkCommandBuffer cmd, VkSemaphore waitSemaphore,
                        uint64_t waitValue, VkFence signalFence);

    VkSemaphore getComputeTimeline() const { return m_computeTimeline; }
    VkQueue     getComputeQueue()    const { return m_computeQueue; }
    VkQueue     getGraphicsQueue()   const { return m_graphicsQueue; }
    VkQueue     getTransferQueue()   const { return m_transferQueue; }

    void destroy();

private:
    VkDevice    m_device        = VK_NULL_HANDLE;
    VkQueue     m_computeQueue  = VK_NULL_HANDLE;
    VkQueue     m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue     m_transferQueue = VK_NULL_HANDLE;
    VkSemaphore m_computeTimeline = VK_NULL_HANDLE; // VK_KHR_timeline_semaphore
};

} // namespace glory
