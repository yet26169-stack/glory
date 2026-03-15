#pragma once

#include "renderer/Device.h"
#include "renderer/Image.h"
#include "renderer/RenderFormats.h"

#include <vulkan/vulkan.h>
#include <memory>

namespace glory {

class HDRFramebuffer {
public:
    void init(const Device& device, uint32_t width, uint32_t height,
              VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT,
              VkFormat depthFormat = VK_FORMAT_D32_SFLOAT);

    void recreate(uint32_t width, uint32_t height);

    VkImageView     colorView()   const { return m_colorImage.getImageView(); }
    VkImageView     colorCopyView() const { return m_colorCopyImage.getImageView(); }
    VkImageView     depthView()   const { return m_depthImage.getImageView(); }
    VkImageView     depthAttachmentView() const { return m_depthAttachmentView; }
    VkImageView     characterDepthView() const { return m_characterDepthImage.getImageView(); }
    VkSampler       sampler()     const { return m_sampler; }

    VkImage         colorImage()  const { return m_colorImage.getImage(); }
    VkImage         depthImage()  const { return m_depthImage.getImage(); }
    VkImage         charDepthImage() const { return m_characterDepthImage.getImage(); }

    VkFormat        colorFormat() const { return m_colorFormat; }
    VkFormat        depthFormat() const { return m_depthFormat; }
    uint32_t        width()  const { return m_width; }
    uint32_t        height() const { return m_height; }

    // RenderFormats for pipeline creation — used by all sub-renderers
    RenderFormats   mainFormats() const;   // HDR clear pass (2 color + depth)
    RenderFormats   loadFormats() const;   // HDR load pass (same formats)

    void copyColor(VkCommandBuffer cmd);

    void destroy();

private:
    const Device* m_device = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    VkFormat m_colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat m_depthFormat = VK_FORMAT_D32_SFLOAT;

    Image m_colorImage;
    Image m_colorCopyImage;
    Image m_depthImage;
    Image m_characterDepthImage;
    // Separate depth view that includes VK_IMAGE_ASPECT_STENCIL_BIT for
    // framebuffer attachment use when a depth-stencil format is active.
    // depthView() keeps returning the depth-only view for sampler reads.
    VkImageView m_depthAttachmentView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    static bool hasStencil(VkFormat fmt) {
        return fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ||
               fmt == VK_FORMAT_D24_UNORM_S8_UINT;
    }

    void createImages();
    void createSampler();
};

} // namespace glory
