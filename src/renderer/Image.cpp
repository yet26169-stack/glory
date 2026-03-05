#include "renderer/Image.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

namespace glory {

Image::Image(const Device& device, uint32_t width, uint32_t height,
             VkFormat format, VkImageUsageFlags usage,
             VkImageAspectFlags aspectFlags)
    : m_vkDevice(device.getDevice())
    , m_allocator(device.getAllocator())
    , m_format(format)
{
    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.extent        = { width, height, 1 };
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = 1;
    imgCI.format        = format;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgCI.usage         = usage;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;

    // When a dedicated transfer queue exists the image may be written from the
    // transfer family and read from the graphics family.  Use CONCURRENT sharing
    // so no explicit queue-family ownership transfer is required.
    uint32_t queueFamilyIndices[] = {
        device.getQueueFamilies().graphicsFamily.value(),
        device.getQueueFamilies().transferFamily.value()
    };
    if (device.hasDedicatedTransfer()) {
        imgCI.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        imgCI.queueFamilyIndexCount = 2;
        imgCI.pQueueFamilyIndices   = queueFamilyIndices;
    } else {
        imgCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VK_CHECK(vmaCreateImage(m_allocator, &imgCI, &allocCI,
                            &m_image, &m_allocation, nullptr),
             "Failed to create image");

    VkImageViewCreateInfo viewCI{};
    viewCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image    = m_image;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format   = format;
    viewCI.subresourceRange.aspectMask     = aspectFlags;
    viewCI.subresourceRange.baseMipLevel   = 0;
    viewCI.subresourceRange.levelCount     = 1;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount     = 1;

    VK_CHECK(vkCreateImageView(m_vkDevice, &viewCI, nullptr, &m_imageView),
             "Failed to create image view");
}

Image::~Image() { destroy(); }

Image::Image(Image&& other) noexcept
    : m_vkDevice(other.m_vkDevice), m_allocator(other.m_allocator)
    , m_image(other.m_image), m_allocation(other.m_allocation)
    , m_imageView(other.m_imageView), m_format(other.m_format)
{
    other.m_vkDevice   = VK_NULL_HANDLE;
    other.m_allocator  = VK_NULL_HANDLE;
    other.m_image      = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_imageView  = VK_NULL_HANDLE;
}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        destroy();
        m_vkDevice   = other.m_vkDevice;
        m_allocator  = other.m_allocator;
        m_image      = other.m_image;
        m_allocation = other.m_allocation;
        m_imageView  = other.m_imageView;
        m_format     = other.m_format;
        other.m_vkDevice   = VK_NULL_HANDLE;
        other.m_allocator  = VK_NULL_HANDLE;
        other.m_image      = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
        other.m_imageView  = VK_NULL_HANDLE;
    }
    return *this;
}

void Image::destroy() {
    if (m_imageView != VK_NULL_HANDLE && m_vkDevice != VK_NULL_HANDLE) {
        vkDestroyImageView(m_vkDevice, m_imageView, nullptr);
        m_imageView = VK_NULL_HANDLE;
    }
    if (m_image != VK_NULL_HANDLE && m_allocator != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_image, m_allocation);
        m_image      = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
    }
}

} // namespace glory
