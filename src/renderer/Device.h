#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <mutex>
#include <optional>
#include <vector>

namespace glory {

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    std::optional<uint32_t> transferFamily; // dedicated DMA queue (may equal graphicsFamily)
    std::optional<uint32_t> computeFamily;  // async compute queue (may equal graphicsFamily)

    bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR        capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR>   presentModes;
};

class Device {
public:
    Device(VkInstance instance, VkSurfaceKHR surface);
    ~Device();

    Device(const Device&)            = delete;
    Device& operator=(const Device&) = delete;

    void cleanup();

    VkDevice         getDevice()              const { return m_device; }
    VkPhysicalDevice getPhysicalDevice()      const { return m_physicalDevice; }
    VkQueue          getGraphicsQueue()       const { return m_graphicsQueue; }
    VkQueue          getPresentQueue()        const { return m_presentQueue; }
    VkQueue          getTransferQueue()       const { return m_transferQueue; }
    VkQueue          getComputeQueue()        const { return m_computeQueue; }
    // True if the transfer queue is a distinct DMA queue (not shared with graphics)
    bool             hasDedicatedTransfer()      const { return m_dedicatedTransfer; }
    bool             hasDedicatedCompute()       const { return m_dedicatedCompute; }
    bool             hasDrawIndirectCount()      const { return m_drawIndirectCountSupported; }
    VmaAllocator     getAllocator()           const { return m_allocator; }
    VkCommandPool    getTransferCommandPool() const { return m_transferCommandPool; }
    VkCommandPool    getGraphicsCommandPool() const { return m_graphicsCommandPool; }
    VkCommandPool    getComputeCommandPool()  const { return m_computeCommandPool; }

    QueueFamilyIndices      getQueueFamilies()      const { return m_indices; }
    SwapchainSupportDetails querySwapchainSupport() const;
    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) const;

    // Thread-safe queue submission wrappers.  These serialise access to each
    // VkQueue handle so that buildScene() can safely run on a background
    // thread while the main thread keeps rendering the loading screen.
    VkResult submitGraphics(uint32_t submitCount, const VkSubmitInfo2* submits,
                            VkFence fence = VK_NULL_HANDLE) const;
    VkResult submitTransfer(uint32_t submitCount, const VkSubmitInfo2* submits,
                            VkFence fence = VK_NULL_HANDLE) const;
    VkResult submitCompute(uint32_t submitCount, const VkSubmitInfo2* submits,
                           VkFence fence = VK_NULL_HANDLE) const;
    VkResult present(const VkPresentInfoKHR* presentInfo) const;

    // Legacy vkQueueSubmit (used by ImpostorSystem)
    VkResult submitGraphicsLegacy(uint32_t submitCount, const VkSubmitInfo* submits,
                                  VkFence fence = VK_NULL_HANDLE) const;

    VkResult graphicsQueueWaitIdle() const;
    VkResult transferQueueWaitIdle() const;
    VkResult computeQueueWaitIdle()  const;

    // Lock guard accessors for command pool thread safety.
    // Callers must hold the lock while using command buffers from the pool.
    std::unique_lock<std::mutex> lockTransferPool() const {
        return std::unique_lock(m_transferPoolMutex);
    }
    std::unique_lock<std::mutex> lockGraphicsPool() const {
        return std::unique_lock(m_graphicsPoolMutex);
    }

    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates,
                                 VkImageTiling tiling,
                                 VkFormatFeatureFlags features) const;
    VkFormat findDepthFormat() const;

private:
    VkInstance       m_instance              = VK_NULL_HANDLE;
    VkSurfaceKHR     m_surface               = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice        = VK_NULL_HANDLE;
    VkDevice         m_device                = VK_NULL_HANDLE;
    VkQueue          m_graphicsQueue         = VK_NULL_HANDLE;
    VkQueue          m_presentQueue          = VK_NULL_HANDLE;
    VkQueue          m_transferQueue         = VK_NULL_HANDLE;
    VkQueue          m_computeQueue          = VK_NULL_HANDLE;
    bool             m_dedicatedTransfer            = false;
    bool             m_dedicatedCompute             = false;
    bool             m_drawIndirectCountSupported   = false;
    VmaAllocator     m_allocator             = VK_NULL_HANDLE;
    VkCommandPool    m_transferCommandPool   = VK_NULL_HANDLE;
    VkCommandPool    m_graphicsCommandPool   = VK_NULL_HANDLE;
    VkCommandPool    m_computeCommandPool    = VK_NULL_HANDLE;
    QueueFamilyIndices m_indices;
    bool m_cleaned = false;

    // Mutexes for thread-safe queue access (mutable because submit methods
    // are logically const — they don't change Device state).
    mutable std::mutex m_graphicsQueueMutex;
    mutable std::mutex m_transferQueueMutex;
    mutable std::mutex m_computeQueueMutex;

    // Mutexes for command pool thread safety — each VkCommandPool must only
    // be accessed from one thread at a time.
    mutable std::mutex m_transferPoolMutex;
    mutable std::mutex m_graphicsPoolMutex;

    void pickPhysicalDevice();
    void createLogicalDevice();
    void createAllocator();
    void createTransferCommandPool();
    void createGraphicsCommandPool();
    void createComputeCommandPool();

    int  rateDeviceSuitability(VkPhysicalDevice device) const;
    bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;

    static const std::vector<const char*>& getRequiredExtensions();
};

} // namespace glory
