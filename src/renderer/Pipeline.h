#pragma once

#include "renderer/Image.h"
#include "renderer/RenderFormats.h"

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace glory {

class Device;
class Swapchain;

class Pipeline {
public:
    Pipeline(const Device& device, const Swapchain& swapchain,
             VkDescriptorSetLayout descriptorSetLayout,
             const RenderFormats& formats);
    ~Pipeline();

    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void cleanup();

    VkPipeline       getGraphicsPipeline() const { return m_graphicsPipeline; }
    VkPipeline       getWireframePipeline() const { return m_wireframePipeline; }
    VkPipelineLayout getPipelineLayout()   const { return m_pipelineLayout; }
    const RenderFormats& getRenderFormats() const { return m_formats; }

private:
    const Device& m_device;
    RenderFormats m_formats;

    VkPipelineLayout           m_pipelineLayout   = VK_NULL_HANDLE;
    VkPipeline                 m_graphicsPipeline  = VK_NULL_HANDLE;
    VkPipeline                 m_wireframePipeline = VK_NULL_HANDLE;
    bool m_cleaned = false;

    void createGraphicsPipeline(VkExtent2D extent, VkDescriptorSetLayout descriptorSetLayout);

    VkShaderModule createShaderModule(const std::vector<char>& code) const;
    static std::vector<char> readFile(const std::string& filepath);
};

} // namespace glory
