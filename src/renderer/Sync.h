#pragma once

#include <vulkan/vulkan.h>

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
    VkFence         getInFlightFence(uint32_t frame)            const { return m_inFlightFences[frame]; }

    // Indexed by swapchain image index (not frame index) to avoid
    // reusing a semaphore still held by the presentation engine.
    VkSemaphore     getRenderFinishedSemaphore(uint32_t imageIndex) const { return m_renderFinished[imageIndex]; }

private:
    const Device& m_device;

    VkCommandPool              m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<VkSemaphore>     m_imageAvailable;
    std::vector<VkSemaphore>     m_renderFinished;  // per swapchain image
    std::vector<VkFence>         m_inFlightFences;
    bool m_cleaned = false;

    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects(uint32_t swapchainImageCount);
};

} // namespace glory
