#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "renderer/Texture.h"
#include "renderer/Buffer.h"
#include "renderer/RenderFormats.h"

namespace glory {

class Device;

/// A reusable renderer for sprite-atlas-based VFX animations.
/// Follows the same pattern as ClickIndicatorRenderer: descriptor set with
/// combined image sampler, billboard quad, push-constant frame indexing.
/// Supports both alpha-blend and additive-blend pipelines.
class SpriteEffectRenderer {
public:
    SpriteEffectRenderer() = default;
    ~SpriteEffectRenderer();

    void init(const Device& device, const RenderFormats& formats);
    void destroy();

    /// Register a new effect type backed by a texture atlas.
    /// Returns an effect ID for use with spawn().
    uint32_t registerEffect(const std::string& name,
                            const std::string& atlasPath,
                            int gridCount,
                            int frameCount,
                            float duration,
                            bool additive);

    /// Spawn an active instance of the given effect at a world position.
    void spawn(uint32_t effectId, const glm::vec3& position, float size,
               const glm::vec4& tint = glm::vec4(1.0f));

    void update(float dt);
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                const glm::vec3& cameraPos);

    static constexpr int MAX_INSTANCES = 32;

private:
    struct PushConstants {
        glm::mat4 viewProj;    // 64
        glm::vec3 center;      // 12
        float     size;        //  4
        int       frameIndex;  //  4
        int       gridCount;   //  4
        int       pad[2];      //  8
        glm::vec4 tint;        // 16
    };  // = 112 B

    struct EffectType {
        std::string name;
        std::unique_ptr<Texture> atlas;
        VkDescriptorSet descSet = VK_NULL_HANDLE;
        int   gridCount  = 8;
        int   frameCount = 48;
        float duration   = 3.0f;
        bool  additive   = true;
    };

    struct Instance {
        uint32_t  effectId;
        glm::vec3 position;
        float     elapsed  = 0.0f;
        float     duration = 3.0f;
        float     size     = 4.0f;
        glm::vec4 tint     {1.0f};
    };

    const Device* m_device = nullptr;

    // Descriptor resources (shared layout + pool, per-effect sets)
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;

    // Two pipelines: alpha-blend and additive-blend
    VkPipelineLayout m_pipelineLayout     = VK_NULL_HANDLE;
    VkPipeline       m_alphaPipeline      = VK_NULL_HANDLE;
    VkPipeline       m_additivePipeline   = VK_NULL_HANDLE;

    Buffer m_vertexBuffer;

    std::vector<EffectType> m_effects;
    std::vector<Instance>   m_active;

    void createDescriptorResources();
    void createPipelines(const RenderFormats& formats);
    void createVertexBuffer();
    VkDescriptorSet allocateDescSetForTexture(const Texture& tex);
};

} // namespace glory
