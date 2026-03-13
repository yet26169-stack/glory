#pragma once

#include "renderer/Device.h"
#include "renderer/Texture.h"
#include "renderer/Mesh.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace glory {

class Model;

class GroundDecalRenderer {
public:
    struct DecalDef {
        std::string id;
        std::string texturePath;  // "" = procedural circle
        float duration = 3.0f;
        float fadeInTime = 0.1f;
        float fadeOutTime = 0.5f;
        float rotationSpeed = 0.0f;  // rad/s
        glm::vec4 color = {1,1,1,1};
        bool additive = false;
    };

    GroundDecalRenderer(const Device& device, VkRenderPass renderPass);
    ~GroundDecalRenderer();

    void registerDecal(const DecalDef& def);

    // Spawn a ground decal. Returns handle for early removal.
    uint32_t spawn(const std::string& decalDefId, glm::vec3 center,
                   float radius, float rotation = 0.0f);

    void update(float dt);

    // Render BEFORE transparent VFX, AFTER opaque geometry.
    void render(VkCommandBuffer cmd, const glm::mat4& viewProj, float appTime);

    void destroy(uint32_t handle);
    void destroyAll();

    static constexpr int MAX_DECALS = 64;

private:
    struct DecalInstance {
        uint32_t handle;
        const DecalDef* def;
        glm::vec3 center;
        float radius;
        float rotation;
        float elapsed = 0.0f;
        Texture* texture = nullptr;
    };

    struct DecalPC {
        glm::mat4 viewProj;  // 64B
        glm::vec3 center;    // 12B
        float     radius;    //  4B
        float     rotation;  //  4B
        float     alpha;     //  4B
        float     elapsed;   //  4B
        float     appTime;   //  4B
        glm::vec4 color;     // 16B
    }; // Total: 112B (Fits in 128B limit)

    const Device& m_device;
    VkRenderPass m_renderPass;

    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_alphaPipeline = VK_NULL_HANDLE;
    VkPipeline            m_additivePipeline = VK_NULL_HANDLE;

    std::unique_ptr<Model> m_quadMesh;
    std::unique_ptr<Texture> m_defaultTexture;

    std::unordered_map<std::string, DecalDef> m_defs;
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_textureCache;
    std::vector<DecalInstance> m_activeDecals;
    std::unordered_map<uint32_t, VkDescriptorSet> m_descSets;

    uint32_t m_nextHandle = 1;

    void createPipelines();
    void createDescriptorLayout();
    VkShaderModule createShaderModule(const std::vector<char>& code);
    Texture* getOrLoadTexture(const std::string& path);
    VkDescriptorSet getOrCreateDescriptorSet(Texture* tex);
};

} // namespace glory
