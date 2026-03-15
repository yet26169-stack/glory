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
    createRenderPass();
    createLoadRenderPass();
    createFramebuffer();
    createSampler();
}

void HDRFramebuffer::recreate(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;

    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device->getDevice(), m_framebuffer, nullptr);
    }
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
    createFramebuffer();
}

void HDRFramebuffer::copyColor(VkCommandBuffer cmd) {
    // 1. Transition original color from SHADER_READ_ONLY_OPTIMAL to TRANSFER_SRC_OPTIMAL
    // Wait, after render pass it is in finalLayout = SHADER_READ_ONLY_OPTIMAL.
    
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.image = m_colorImage.getImage();
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // 2. Transition copy from UNDEFINED/SHADER_READ_ONLY to TRANSFER_DST_OPTIMAL
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // We don't care about previous contents
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image = m_colorCopyImage.getImage();

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

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
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.image = m_colorImage.getImage();

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // 5. Transition copy to SHADER_READ_ONLY_OPTIMAL
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.image = m_colorCopyImage.getImage();

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void HDRFramebuffer::destroy() {
    if (!m_device) return;

    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device->getDevice(), m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }

    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device->getDevice(), m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }

    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device->getDevice(), m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }

    if (m_loadRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device->getDevice(), m_loadRenderPass, nullptr);
        m_loadRenderPass = VK_NULL_HANDLE;
    }

    m_colorImage = Image{};
    m_colorCopyImage = Image{};
    // Destroy the extra depth-stencil attachment view (it's different from the
    // depth-only sample view owned by m_depthImage when hasStencil() is true).
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
    // the framebuffer attachment can perform stencil operations.
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

void HDRFramebuffer::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_colorFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = m_depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = hasStencil(m_depthFormat)
                                     ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                     : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription charDepthAttachment{};
    charDepthAttachment.format         = VK_FORMAT_R32_SFLOAT;
    charDepthAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    charDepthAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    charDepthAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    charDepthAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    charDepthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    charDepthAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    charDepthAttachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference charDepthRef{};
    charDepthRef.attachment = 2;
    charDepthRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRefs[2] = {colorAttachmentRef, charDepthRef};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 2;
    subpass.pColorAttachments    = colorRefs;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    std::array<VkSubpassDependency, 2> dependencies;
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    std::array<VkAttachmentDescription, 3> attachments = {colorAttachment, depthAttachment, charDepthAttachment};
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();

    VK_CHECK(vkCreateRenderPass(m_device->getDevice(), &renderPassInfo, nullptr, &m_renderPass),
             "Create HDR render pass");
}

void HDRFramebuffer::createLoadRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_colorFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = m_depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentDescription charDepthAttachmentLoad{};
    charDepthAttachmentLoad.format         = VK_FORMAT_R32_SFLOAT;
    charDepthAttachmentLoad.samples        = VK_SAMPLE_COUNT_1_BIT;
    charDepthAttachmentLoad.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    charDepthAttachmentLoad.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    charDepthAttachmentLoad.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    charDepthAttachmentLoad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    charDepthAttachmentLoad.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    charDepthAttachmentLoad.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    // charDepth is loaded (not written) during this pass; reference it so both
    // render passes have the same subpass structure and share a compatible framebuffer.
    VkAttachmentReference charDepthLoadRef{};
    charDepthLoadRef.attachment = 2;
    // GENERAL allows the image to be both referenced as a color attachment (write
    // mask=0 — no actual writes) and sampled by InkingPass simultaneously, avoiding
    // the undefined-behaviour layout conflict that COLOR_ATTACHMENT_OPTIMAL would cause.
    charDepthLoadRef.layout     = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference colorRefsLoad[2] = {colorAttachmentRef, charDepthLoadRef};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 2;
    subpass.pColorAttachments    = colorRefsLoad;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    std::array<VkSubpassDependency, 2> dependencies;
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    std::array<VkAttachmentDescription, 3> attachments = {colorAttachment, depthAttachment, charDepthAttachmentLoad};
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    renderPassInfo.pDependencies = dependencies.data();

    VK_CHECK(vkCreateRenderPass(m_device->getDevice(), &renderPassInfo, nullptr, &m_loadRenderPass),
             "Create HDR load render pass");
}

void HDRFramebuffer::createFramebuffer() {
    std::array<VkImageView, 3> attachments = {
        m_colorImage.getImageView(),
        m_depthAttachmentView,
        m_characterDepthImage.getImageView()
    };

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    // Use the main render pass as the framebuffer's reference so it is compatible
    // with both m_renderPass and m_loadRenderPass (both now have the same subpass structure).
    framebufferInfo.renderPass = m_renderPass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = m_width;
    framebufferInfo.height = m_height;
    framebufferInfo.layers = 1;

    VK_CHECK(vkCreateFramebuffer(m_device->getDevice(), &framebufferInfo, nullptr, &m_framebuffer),
             "Create HDR framebuffer");
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
