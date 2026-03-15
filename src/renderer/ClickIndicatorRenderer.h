#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <memory>
#include <string>

#include "renderer/Texture.h"
#include "renderer/Buffer.h"
#include "renderer/RenderFormats.h"

namespace glory {

class Device;

class ClickIndicatorRenderer {
public:
    ClickIndicatorRenderer(const Device& device, const RenderFormats& formats);
    ~ClickIndicatorRenderer();

    void render(VkCommandBuffer cmd, const glm::mat4& viewProj, 
                const glm::vec3& center, float t, float size,
                const glm::vec4& tint = glm::vec4(1.0f));

private:
    const Device& m_device;
    
    std::unique_ptr<Texture> m_texture;
    
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descSet = VK_NULL_HANDLE;
    
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    
    Buffer m_vertexBuffer;
    
    struct PushConstants {
        glm::mat4 viewProj;
        glm::vec3 center;
        float size;
        int frameIndex;
        int gridCount;
        int padding[2];
        glm::vec4 tint;
    };
    
    void createDescriptorSet();
    void createPipeline(const RenderFormats& formats);
    void createVertexBuffer();
};

} // namespace glory
