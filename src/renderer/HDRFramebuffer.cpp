#include "renderer/HDRFramebuffer.h"
#include "renderer/VkCheck.h"

#include <array>

namespace glory {

void HDRFramebuffer::init(const Device& device, uint32_t width, uint32_t height,
                         VkFormat colorFormat, VkFormat depthFormat) {
    m_device = &device;
    m_width = width;
    m_height = height;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;

    createImages();
    createSampler();
}

void HDRFramebuffer::recreate(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    if (m_depthAttachmentView != VK_NULL_HANDLE &&
        m_depthAttachmentView != m_depthImage.getImageView()) {
        vkDestroyImageView(m_device->getDevice(), m_depthAttachmentView, nullptr);
    }
    m_depthAttachmentView = VK_NULL_HANDLE;
    m_colorImage = Image{};
    m_colorCopyImage = Image{};
    m_depthImage = Image{};
    m_characterDepthImage = Image{};

    createImages();
}

RenderFormats HDRFramebuffer::mainFormats() const {
    return RenderFormats::hdrMain(m_colorFormat, m_depthFormat, VK_FORMAT_R32_SFLOAT);
}

RenderFormats HDRFramebuffer::loadFormats() const {
    // Same formats as main pass — the difference is LOAD vs CLEAR ops,
    // which are specified at vkCmdBeginRendering time, not in the formats.
    return RenderFormats::hdrMain(m_colorFormat, m_depthFormat, VK_FORMAT_R32_SFLOAT);
}

void HDRFramebuffer::copyColor(VkCommandBuffer cmd) {
    // 1. Transition original color from SHADER_READ_ONLY_OPTIMAL to TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.image = m_colorImage.getImage();
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &depInfo);

    // 2. Transition copy from UNDEFINED/SHADER_READ_ONLY to TRANSFER_DST_OPTIMAL
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = m_colorCopyImage.getImage();

    vkCmdPipelineBarrier2(cmd, &depInfo);

    // 3. Copy
    VkImageCopy copyRegion{};
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.layerCount = 1;
    copyRegion.extent.width = m_width;
    copyRegion.extent.height = m_height;
    copyRegion.extent.depth = 1;

    vkCmdCopyImage(cmd, m_colorImage.getImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_colorCopyImage.getImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &copyRegion);

    // 4. Transition original back to COLOR_ATTACHMENT_OPTIMAL (for distortion pass)
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.image = m_colorImage.getImage();

    vkCmdPipelineBarrier2(cmd, &depInfo);

    // 5. Transition copy to SHADER_READ_ONLY_OPTIMAL
    barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.image = m_colorCopyImage.getImage();

    vkCmdPipelineBarrier2(cmd, &depInfo);
}

void HDRFramebuffer::destroy() {
    if (!m_device) return;

    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device->getDevice(), m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }

    m_colorImage = Image{};
    m_colorCopyImage = Image{};
    if (m_depthAttachmentView != VK_NULL_HANDLE &&
        m_depthAttachmentView != m_depthImage.getImageView()) {
        vkDestroyImageView(m_device->getDevice(), m_depthAttachmentView, nullptr);
    }
    m_depthAttachmentView = VK_NULL_HANDLE;
    m_depthImage = Image{};
    m_characterDepthImage = Image{};
}

void HDRFramebuffer::createImages() {
    // Usage: color attachment + shader sampling + transfer src
    m_colorImage = Image(*m_device, m_width, m_height, m_colorFormat,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);

    // Usage: shader sampling + transfer dst
    m_colorCopyImage = Image(*m_device, m_width, m_height, m_colorFormat,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT);

    m_depthImage = Image(*m_device, m_width, m_height, m_depthFormat,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT);

    // For depth-stencil formats create a second view covering BOTH aspects so
    // dynamic rendering's depth attachment can perform stencil operations.
    if (m_depthAttachmentView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device->getDevice(), m_depthAttachmentView, nullptr);
        m_depthAttachmentView = VK_NULL_HANDLE;
    }
    if (hasStencil(m_depthFormat)) {
        VkImageViewCreateInfo vci{};
        vci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image                           = m_depthImage.getImage();
        vci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                          = m_depthFormat;
        vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        vci.subresourceRange.baseMipLevel   = 0;
        vci.subresourceRange.levelCount     = 1;
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(m_device->getDevice(), &vci, nullptr, &m_depthAttachmentView),
                 "depth-stencil attachment view");
    } else {
        m_depthAttachmentView = m_depthImage.getImageView();
    }

    m_characterDepthImage = Image(*m_device, m_width, m_height, VK_FORMAT_R32_SFLOAT,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
}

void HDRFramebuffer::createSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    VK_CHECK(vkCreateSampler(m_device->getDevice(), &samplerInfo, nullptr, &m_sampler),
             "Create HDR sampler");
}

} // namespace glory
