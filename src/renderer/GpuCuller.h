#pragma once
// ── GPU Frustum Culler ────────────────────────────────────────────────────
// Runs a compute shader each frame that tests every registered draw object
// against the camera frustum and writes a compact VkDrawIndexedIndirectCommand
// list ready for vkCmdDrawIndexedIndirectCount.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include "renderer/Buffer.h"

namespace glory {

class Device;

// Must match the GLSL DrawObject struct (std430) in cull.comp — 160 bytes
struct CullObject {
    // AABB (std430 vec3 + float pad = 16 bytes each)
    glm::vec3 aabbMin;
    float     _pad0 = 0.f;
    glm::vec3 aabbMax;
    float     _pad1 = 0.f;
    // VkDrawIndexedIndirectCommand fields (5 × 4 = 20 bytes) + 3 × uint pad = 32 bytes total
    uint32_t  indexCount    = 0;
    uint32_t  instanceCount = 1;
    uint32_t  firstIndex    = 0;
    int32_t   vertexOffset  = 0;
    uint32_t  firstInstance = 0;
    uint32_t  _drawPad[3]   = {};
    // Instance data written to output buffer for GPU-driven draw merging
    glm::mat4 modelMatrix   = glm::mat4(1.0f); // 64 bytes
    glm::vec4 tintAndFlags  = glm::vec4(1,1,1,1); // tint RGB + reserved
    uint32_t  texDiffuse    = 0;
    uint32_t  texNormal     = 0;
    float     shininess     = 32.0f;
    float     metallic      = 0.0f;
    // total: 160 bytes (8+8+32+64+16+16 = 144 + 16 pad... check)
};

class GpuCuller {
public:
    GpuCuller() = default;
    ~GpuCuller() { destroy(); }

    GpuCuller(const GpuCuller&)            = delete;
    GpuCuller& operator=(const GpuCuller&) = delete;

    void init(const Device& device, uint32_t framesInFlight,
              uint32_t maxObjects = 4096);
    void destroy();

    // Register a drawable — returns slot index used to update it later
    uint32_t addObject(const CullObject& obj);
    // Update an existing slot (e.g. after skinning moves AABB, or instance count changes)
    void     updateObject(uint32_t slot, const CullObject& obj);
    // Update the set of registered objects wholesale (replaces all)
    void     setObjects(const std::vector<CullObject>& objs);

    // Upload new frustum planes + dispatch the compute shader
    // vp: combined viewProjection matrix (row-major, standard GLM)
    void dispatch(VkCommandBuffer cmd, const glm::mat4& vp, uint32_t frameIdx);

    // Buffer containing VkDrawIndexedIndirectCommand[] after dispatch
    // Layout: [uint32 drawCount][DrawCmd cmd0][DrawCmd cmd1]...
    VkBuffer outputBuffer(uint32_t frameIdx) const;
    // Byte offset of the first command in the output buffer (after the count)
    VkDeviceSize commandOffset() const { return sizeof(uint32_t); }

    // Instance buffer written in the same pass as culling (parallel output).
    // Layout: [InstanceData inst0][InstanceData inst1]...
    // Only valid after dispatch(); index i corresponds to draw command i.
    VkBuffer instanceBuffer(uint32_t frameIdx) const;

    uint32_t maxObjects() const { return m_maxObjects; }

private:
    struct FrameData {
        // Input: per-object AABB + draw args
        Buffer        objectBuffer;
        // Output: drawCount(uint) + VkDrawIndexedIndirectCommand[]
        Buffer        outputBuffer;
        // Output: per-surviving-instance data (written alongside draw commands)
        Buffer        instanceBuffer;
        // Frustum UBO
        Buffer        frustumBuffer;

        VkDescriptorSet descSet       = VK_NULL_HANDLE;
    };

    struct FrustumUBO {
        glm::vec4 planes[6];
        uint32_t  objectCount = 0;
        uint32_t  maxCommands = 0;
        float     _pad[2]     = {};
    };

    void createPipeline();
    void createDescriptorPool(uint32_t frames);
    void createFrameData(uint32_t frames);

    static glm::vec4 normalizePlane(glm::vec4 p);
    static void extractFrustumPlanes(const glm::mat4& vp, glm::vec4 out[6]);

    const Device*    m_device   = nullptr;
    VkDevice         m_vkDevice = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descLayout  = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool    = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeLayout  = VK_NULL_HANDLE;
    VkPipeline            m_pipeline    = VK_NULL_HANDLE;

    std::vector<FrameData> m_frames;
    std::vector<CullObject> m_objects;

    uint32_t m_maxObjects  = 0;
    uint32_t m_framesCount = 0;
};

} // namespace glory
