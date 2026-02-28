#include "renderer/Swapchain.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>

namespace glory {

Swapchain::Swapchain(const Device& device, VkSurfaceKHR surface, VkExtent2D windowExtent)
    : m_device(device), m_surface(surface)
{
    create(windowExtent);
    createImageViews();
}

Swapchain::~Swapchain() { cleanup(); }

void Swapchain::cleanup() {
    if (m_cleaned) return;
    m_cleaned = true;
    destroyImageViews();
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device.getDevice(), m_swapchain, nullptr);
        spdlog::info("Swapchain destroyed");
    }
}

void Swapchain::recreate(VkExtent2D newExtent) {
    destroyImageViews();
    VkSwapchainKHR old = m_swapchain;
    create(newExtent, old);
    createImageViews();
    vkDestroySwapchainKHR(m_device.getDevice(), old, nullptr);
    spdlog::info("Swapchain recreated ({}x{})", m_extent.width, m_extent.height);
}

// ── Private ─────────────────────────────────────────────────────────────────
void Swapchain::create(VkExtent2D windowExtent, VkSwapchainKHR oldSwapchain) {
    auto support = m_device.querySwapchainSupport();

    auto fmt  = chooseFormat(support.formats);
    auto mode = choosePresentMode(support.presentModes);
    auto ext  = chooseExtent(support.capabilities, windowExtent);

    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 &&
        imageCount > support.capabilities.maxImageCount)
    {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = m_surface;
    ci.minImageCount    = imageCount;
    ci.imageFormat      = fmt.format;
    ci.imageColorSpace  = fmt.colorSpace;
    ci.imageExtent      = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    auto indices = m_device.getQueueFamilies();
    uint32_t queueFamilyIndices[] = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value()
    };

    if (indices.graphicsFamily != indices.presentFamily) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = queueFamilyIndices;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    ci.preTransform   = support.capabilities.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode    = mode;
    ci.clipped        = VK_TRUE;
    ci.oldSwapchain   = oldSwapchain;

    VK_CHECK(vkCreateSwapchainKHR(m_device.getDevice(), &ci, nullptr, &m_swapchain),
             "Failed to create swapchain");

    vkGetSwapchainImagesKHR(m_device.getDevice(), m_swapchain, &imageCount, nullptr);
    m_images.resize(imageCount);
    vkGetSwapchainImagesKHR(m_device.getDevice(), m_swapchain, &imageCount, m_images.data());

    m_imageFormat = fmt.format;
    m_extent      = ext;

    spdlog::info("Swapchain created ({}x{}, {} images, format {})",
                 ext.width, ext.height, imageCount, static_cast<int>(m_imageFormat));
}

void Swapchain::createImageViews() {
    m_imageViews.resize(m_images.size());
    for (size_t i = 0; i < m_images.size(); ++i) {
        VkImageViewCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image    = m_images[i];
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format   = m_imageFormat;
        ci.components = { VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY };
        ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.baseMipLevel   = 0;
        ci.subresourceRange.levelCount     = 1;
        ci.subresourceRange.baseArrayLayer = 0;
        ci.subresourceRange.layerCount     = 1;

        VK_CHECK(vkCreateImageView(m_device.getDevice(), &ci, nullptr, &m_imageViews[i]),
                 "Failed to create image view");
    }
    spdlog::trace("{} image views created", m_imageViews.size());
}

void Swapchain::destroyImageViews() {
    for (auto iv : m_imageViews)
        if (iv != VK_NULL_HANDLE)
            vkDestroyImageView(m_device.getDevice(), iv, nullptr);
    m_imageViews.clear();
}

VkSurfaceFormatKHR Swapchain::chooseFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) const
{
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    return formats[0];
}

VkPresentModeKHR Swapchain::choosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) const
{
    for (auto m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Swapchain::chooseExtent(
    const VkSurfaceCapabilitiesKHR& caps, VkExtent2D want) const
{
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
        return caps.currentExtent;

    want.width  = std::clamp(want.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
    want.height = std::clamp(want.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return want;
}

} // namespace glory
