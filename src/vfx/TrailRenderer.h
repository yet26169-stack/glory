#pragma once

#include "vfx/TrailTypes.h"
#include "renderer/Buffer.h"
#include "renderer/Texture.h"
#include <vulkan/vulkan.h>
#include <memory>
#include <unordered_map>
#include <deque>

namespace glory {

class Device;

class TrailRenderer {
public:
    TrailRenderer(const Device& device, VkRenderPass renderPass);
    ~TrailRenderer();

    void registerTrail(const TrailDef& def);

    // Spawn a trail. Returns a unique handle.
    uint32_t spawn(const std::string& trailDefId, glm::vec3 startPos);

    // Update position of the trail head.
    void updateHead(uint32_t handle, glm::vec3 newHeadPos);

    // Detach trail from source — it will fade out.
    void detach(uint32_t handle);

    // Per-frame update.
    void update(float dt);

    // Render all active trails.
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                const glm::vec3& camRight, const glm::vec3& camUp);

private:
    struct TrailInstance {
        uint32_t handle;
        const TrailDef* def;
        bool detached = false;
        float emitAccum = 0.0f;
        glm::vec3 lastHeadPos;
        
        // Circular buffer would be better, but we'll use a deque for CPU and a linear SSBO for GPU simplicity
        std::deque<GpuTrailPoint> points;
        
        Buffer ssbo;
        VkDescriptorSet descSet;
        Texture* texture = nullptr;
    };

    const Device& m_device;
    VkRenderPass m_renderPass;

    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_alphaPipeline = VK_NULL_HANDLE;
    VkPipeline            m_additivePipeline = VK_NULL_HANDLE;

    std::unordered_map<std::string, TrailDef> m_defs;
    std::vector<std::unique_ptr<TrailInstance>> m_activeTrails;
    uint32_t m_nextHandle = 1;

    std::unique_ptr<Texture> m_whiteTexture;

    void createPipelines();
    void createDescriptorLayout();
    void updateInstanceBuffer(TrailInstance& inst);
    VkShaderModule createShaderModule(const std::vector<char>& code);
};

} // namespace glory
