#pragma once

#include "vfx/MeshEffect.h"
#include "renderer/Device.h"
#include "renderer/Model.h"
#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <unordered_map>

namespace glory {

class MeshEffectRenderer {
public:
    MeshEffectRenderer(const Device& device, VkRenderPass renderPass);
    ~MeshEffectRenderer();

    void registerDef(const MeshEffectDef& def);

    void spawn(const std::string& defId, glm::vec3 position,
               glm::vec3 direction, float scale = 1.0f);

    void update(float dt);

    void render(VkCommandBuffer cmd, const glm::mat4& viewProj, float appTime);

    static constexpr int MAX_INSTANCES = 32;

private:
    const Device& m_device;
    VkRenderPass m_renderPass;

    struct MeshData {
        std::unique_ptr<Model> model;
    };

    struct PipelineKey {
        std::string vert;
        std::string frag;
        bool additive;
        bool operator==(const PipelineKey& o) const {
            return vert == o.vert && frag == o.frag && additive == o.additive;
        }
    };

    struct PipelineKeyHash {
        size_t operator()(const PipelineKey& k) const {
            return std::hash<std::string>{}(k.vert) ^ std::hash<std::string>{}(k.frag) ^ (k.additive ? 1234 : 5678);
        }
    };

    struct PipelineState {
        VkPipelineLayout layout;
        VkPipeline pipeline;
    };

    struct MeshEffectPC {
        glm::mat4  viewProj;
        glm::vec4  color;
        glm::vec4  data;
        glm::vec4  posRot;
        glm::vec4  dir;
    };

    std::unordered_map<std::string, MeshEffectDef> m_defs;
    std::unordered_map<std::string, MeshData> m_meshes;
    std::unordered_map<PipelineKey, PipelineState, PipelineKeyHash> m_pipelines;
    std::vector<MeshEffectInstance> m_active;

    uint32_t m_nextHandle = 1;

    void createPipeline(const PipelineKey& key);
    MeshData* getOrLoadMesh(const std::string& path);
    VkShaderModule createShaderModule(const std::vector<char>& code);
};

} // namespace glory
