#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace glory {

class Device;

class Swapchain {
public:
    Swapchain(const Device& device, VkSurfaceKHR surface, VkExtent2D windowExtent);
    ~Swapchain();

    Swapchain(const Swapchain&)            = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    void cleanup();
    void recreate(VkExtent2D newExtent);

    VkSwapchainKHR              getSwapchain()   const { return m_swapchain; }
    VkFormat                    getImageFormat()  const { return m_imageFormat; }
    VkExtent2D                  getExtent()       const { return m_extent; }
    const std::vector<VkImageView>& getImageViews() const { return m_imageViews; }
    uint32_t                    getImageCount()   const { return static_cast<uint32_t>(m_images.size()); }

private:
    const Device&  m_device;
    VkSurfaceKHR   m_surface;

    VkSwapchainKHR            m_swapchain   = VK_NULL_HANDLE;
    VkFormat                  m_imageFormat{};
    VkExtent2D                m_extent{};
    std::vector<VkImage>      m_images;
    std::vector<VkImageView>  m_imageViews;
    bool m_cleaned = false;

    void create(VkExtent2D windowExtent, VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
    void createImageViews();
    void destroyImageViews();

    VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR   choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const;
    VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D want) const;
};

} // namespace glory
