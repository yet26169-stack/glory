/// PipelineLibrary — VK_EXT_graphics_pipeline_library wrapper.
/// Full implementation requires the extension to be enabled in Device.cpp.
/// See GLORY_ENGINE_AAA_IMPLEMENTATION_PLAN.md Phase 4.5 for enable instructions.
#include "renderer/PipelineLibrary.h"
#include "renderer/VkCheck.h"
#include <spdlog/spdlog.h>

namespace glory {

void PipelineLibrary::init(VkDevice device) {
    m_device = device;
    spdlog::info("PipelineLibrary: initialised (VK_EXT_graphics_pipeline_library)");
}

void PipelineLibrary::precompileVertexInput(
    const std::string& key,
    const VkPipelineVertexInputStateCreateInfo& /*vi*/,
    const VkPipelineInputAssemblyStateCreateInfo& /*ia*/) {
    // TODO: create VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT fragment
    spdlog::debug("PipelineLibrary: queued vertex-input pre-compile '{}'", key);
}

void PipelineLibrary::precompilePreRaster(
    const std::string& key,
    VkShaderModule /*vertShader*/,
    const VkPipelineLayout& /*layout*/,
    VkRenderPass /*renderPass*/) {
    // TODO: create VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT fragment
    spdlog::debug("PipelineLibrary: queued pre-raster pre-compile '{}'", key);
}

VkPipeline PipelineLibrary::link(
    const std::string& vertKey,
    VkShaderModule /*fragShader*/,
    const VkPipelineColorBlendStateCreateInfo& /*blend*/,
    const VkPipelineDepthStencilStateCreateInfo& /*ds*/) {
    // Check cache first
    auto it = m_linked.find(vertKey);
    if (it != m_linked.end()) return it->second;
    // TODO: link pre-compiled vertex + new fragment stages into full pipeline
    spdlog::warn("PipelineLibrary::link '{}': stub — returning VK_NULL_HANDLE", vertKey);
    return VK_NULL_HANDLE;
}

void PipelineLibrary::destroy() {
    for (auto& [k, p] : m_linked)   { if (p) vkDestroyPipeline(m_device, p, nullptr); }
    for (auto& [k, p] : m_fragments){ if (p) vkDestroyPipeline(m_device, p, nullptr); }
    m_linked.clear();
    m_fragments.clear();
}

} // namespace glory
