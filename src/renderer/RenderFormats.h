#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace glory {

// Lightweight descriptor of color/depth/stencil formats for a rendering pass.
// Replaces VkRenderPass in sub-renderer constructor/init signatures so that
// pipelines can be created with VkPipelineRenderingCreateInfo (Vulkan 1.3).
struct RenderFormats {
    static constexpr uint32_t MAX_COLOR_ATTACHMENTS = 4;

    VkFormat colorFormats[MAX_COLOR_ATTACHMENTS] = {};
    uint32_t colorCount    = 0;
    VkFormat depthFormat   = VK_FORMAT_UNDEFINED;
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;

    // Fill a VkPipelineRenderingCreateInfo in-place (caller must keep this
    // struct alive while the pipeline create info references it).
    VkPipelineRenderingCreateInfo pipelineRenderingCI() const {
        VkPipelineRenderingCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ci.colorAttachmentCount    = colorCount;
        ci.pColorAttachmentFormats = colorFormats;
        ci.depthAttachmentFormat   = depthFormat;
        ci.stencilAttachmentFormat = stencilFormat;
        return ci;
    }

    // Convenience factories for common pass configurations
    static RenderFormats hdrMain(VkFormat color, VkFormat depth, VkFormat charDepth) {
        RenderFormats f;
        f.colorFormats[0] = color;
        f.colorFormats[1] = charDepth;
        f.colorCount      = 2;
        f.depthFormat     = depth;
        // Extract stencil from combined depth-stencil formats
        if (depth == VK_FORMAT_D32_SFLOAT_S8_UINT || depth == VK_FORMAT_D24_UNORM_S8_UINT)
            f.stencilFormat = depth;
        return f;
    }

    static RenderFormats depthOnly(VkFormat depth) {
        RenderFormats f;
        f.depthFormat = depth;
        return f;
    }

    static RenderFormats colorOnly(VkFormat color) {
        RenderFormats f;
        f.colorFormats[0] = color;
        f.colorCount      = 1;
        return f;
    }

    static RenderFormats swapchain(VkFormat swapchainFormat) {
        return colorOnly(swapchainFormat);
    }
};

} // namespace glory
