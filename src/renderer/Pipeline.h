#pragma once

#include "renderer/Image.h"

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
             VkRenderPass externalRenderPass = VK_NULL_HANDLE);
    ~Pipeline();

    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void cleanup();
    void recreateFramebuffers(const Swapchain& swapchain);

    VkRenderPass     getRenderPass()       const { return m_renderPass; }
    VkPipeline       getGraphicsPipeline() const { return m_graphicsPipeline; }
    VkPipeline       getWireframePipeline() const { return m_wireframePipeline; }
    VkPipelineLayout getPipelineLayout()   const { return m_pipelineLayout; }
    VkFramebuffer    getFramebuffer(uint32_t index) const { return m_framebuffers[index]; }

private:
    const Device& m_device;
    bool m_ownsRenderPass = true;

    VkRenderPass               m_renderPass       = VK_NULL_HANDLE;
    VkPipelineLayout           m_pipelineLayout   = VK_NULL_HANDLE;
    VkPipeline                 m_graphicsPipeline  = VK_NULL_HANDLE;
    VkPipeline                 m_wireframePipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_framebuffers;
    Image                      m_depthImage;
    VkFormat                   m_depthFormat{};
    bool m_cleaned = false;

    void createRenderPass(VkFormat imageFormat);
    void createGraphicsPipeline(VkExtent2D extent, VkDescriptorSetLayout descriptorSetLayout);
    void createDepthResources(const Swapchain& swapchain);
    void createFramebuffers(const Swapchain& swapchain);
    void destroyFramebuffers();

    VkShaderModule createShaderModule(const std::vector<char>& code) const;
    static std::vector<char> readFile(const std::string& filepath);
};

} // namespace glory
