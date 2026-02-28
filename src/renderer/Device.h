#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <optional>
#include <vector>

namespace glory {

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

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
    VmaAllocator     getAllocator()           const { return m_allocator; }
    VkCommandPool    getTransferCommandPool() const { return m_transferCommandPool; }

    QueueFamilyIndices      getQueueFamilies()      const { return m_indices; }
    SwapchainSupportDetails querySwapchainSupport() const;
    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) const;

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
    VmaAllocator     m_allocator             = VK_NULL_HANDLE;
    VkCommandPool    m_transferCommandPool   = VK_NULL_HANDLE;
    QueueFamilyIndices m_indices;
    bool m_cleaned = false;

    void pickPhysicalDevice();
    void createLogicalDevice();
    void createAllocator();
    void createTransferCommandPool();

    int  rateDeviceSuitability(VkPhysicalDevice device) const;
    bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;

    static const std::vector<const char*>& getRequiredExtensions();
};

} // namespace glory
