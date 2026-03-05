#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <unordered_map>

namespace glory {

/// Pre-compiled pipeline fragment cache using VK_EXT_graphics_pipeline_library.
/// Vertex-input and pre-rasterisation stages are compiled once at startup;
/// final pipelines are linked at runtime (fragment + output stages only),
/// eliminating stalls during gameplay.
///
/// Requires VK_EXT_graphics_pipeline_library. Enable in Device.cpp feature chain.
class PipelineLibrary {
public:
    void init(VkDevice device);

    /// Pre-compile vertex-input stage fragment.
    void precompileVertexInput(const std::string& key,
                                const VkPipelineVertexInputStateCreateInfo& vi,
                                const VkPipelineInputAssemblyStateCreateInfo& ia);

    /// Pre-compile pre-rasterisation stage fragment (vertex shader).
    void precompilePreRaster(const std::string& key,
                              VkShaderModule vertShader,
                              const VkPipelineLayout& layout,
                              VkRenderPass renderPass);

    /// Link a full pipeline from cached fragments + fragment shader (fast).
    VkPipeline link(const std::string& vertKey,
                    VkShaderModule fragShader,
                    const VkPipelineColorBlendStateCreateInfo& blend,
                    const VkPipelineDepthStencilStateCreateInfo& ds);

    void destroy();

private:
    VkDevice m_device = VK_NULL_HANDLE;
    std::unordered_map<std::string, VkPipeline> m_fragments;
    std::unordered_map<std::string, VkPipeline> m_linked;
};

} // namespace glory
