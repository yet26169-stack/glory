#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace glory {

class Device;

class Image {
public:
    Image() = default;
    Image(const Device& device, uint32_t width, uint32_t height,
          VkFormat format, VkImageUsageFlags usage,
          VkImageAspectFlags aspectFlags);
    ~Image();

    Image(const Image&)            = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    VkImage     getImage()     const { return m_image; }
    VkImageView getImageView() const { return m_imageView; }
    VkFormat    getFormat()    const { return m_format; }

private:
    VkDevice      m_vkDevice   = VK_NULL_HANDLE;
    VmaAllocator  m_allocator  = VK_NULL_HANDLE;
    VkImage       m_image      = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkImageView   m_imageView  = VK_NULL_HANDLE;
    VkFormat      m_format{};

    void destroy();
};

} // namespace glory
