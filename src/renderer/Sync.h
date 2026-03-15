#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace glory {

class Device;

class Sync {
public:
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

    Sync(const Device& device, uint32_t swapchainImageCount);
    ~Sync();

    Sync(const Sync&)            = delete;
    Sync& operator=(const Sync&) = delete;

    void cleanup();
    void recreateRenderFinishedSemaphores(uint32_t swapchainImageCount);

    VkCommandBuffer getCommandBuffer(uint32_t frame)            const { return m_commandBuffers[frame]; }
    VkSemaphore     getImageAvailableSemaphore(uint32_t frame)  const { return m_imageAvailable[frame]; }

    // Indexed by swapchain image index (not frame index) to avoid
    // reusing a semaphore still held by the presentation engine.
    VkSemaphore     getRenderFinishedSemaphore(uint32_t imageIndex) const { return m_renderFinished[imageIndex]; }

    // ── Timeline semaphore (replaces VkFence for CPU–GPU frame sync) ─────
    VkSemaphore getTimelineSemaphore() const { return m_timeline; }

    // Block the CPU until the GPU finishes the previous work submitted
    // for the given frame slot.
    void     waitForFrame(uint32_t frame);

    // Increment the monotonic counter, record the new value against
    // this frame slot, and return it (caller passes it to VkSubmitInfo2).
    uint64_t nextSignalValue(uint32_t frame);

private:
    const Device& m_device;

    VkCommandPool                m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkSemaphore>     m_imageAvailable;   // binary, per frame-in-flight
    std::vector<VkSemaphore>     m_renderFinished;   // binary, per swapchain image
    VkSemaphore                  m_timeline = VK_NULL_HANDLE; // timeline, single

    uint64_t m_timelineCounter = 0;
    std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> m_timelineValues{};

    bool m_cleaned = false;

    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects(uint32_t swapchainImageCount);
};

} // namespace glory
