#pragma once

#include "renderer/Device.h"
#include "renderer/Texture.h"
#include "renderer/Model.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <memory>

namespace glory {

struct DistortionDef {
    std::string id;
    float duration = 1.0f;
    float radius = 5.0f;
    float strength = 1.0f;
};

struct DistortionInstance {
    uint32_t handle;
    const DistortionDef* def;
    glm::vec3 position;
    float elapsed = 0.0f;
};

class DistortionRenderer {
public:
    DistortionRenderer(const Device& device, VkRenderPass renderPass, VkImageView sceneColorCopy, VkSampler sampler);
    ~DistortionRenderer();

    void registerDef(const DistortionDef& def);
    uint32_t spawn(const std::string& defId, glm::vec3 position);
    void update(float dt);
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj, float appTime, uint32_t width, uint32_t height);

    void updateDescriptorSet(VkImageView sceneColorCopy);

    static constexpr int MAX_DISTORTIONS = 32;

private:
    const Device& m_device;
    VkRenderPass m_renderPass;
    VkImageView m_sceneColorCopy;
    VkSampler m_sampler;

    struct DistortionPC {
        glm::mat4  viewProj;
        glm::vec3  center;
        float      radius;
        float      strength;
        float      elapsed;
        glm::vec2  screenSize;
    };

    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet    = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline = VK_NULL_HANDLE;

    std::unique_ptr<Model> m_sphereMesh; // Or just a quad

    std::unordered_map<std::string, DistortionDef> m_defs;
    std::vector<DistortionInstance> m_active;
    uint32_t m_nextHandle = 1;

    void createPipeline();
    void createDescriptorSet();
    VkShaderModule createShaderModule(const std::vector<char>& code);
};

} // namespace glory
