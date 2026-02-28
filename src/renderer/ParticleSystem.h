#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <vector>
#include <random>

namespace glory {

class Device;

// GPU particle data layout (matches compute shader std430)
struct GPUParticle {
    glm::vec4 posAndLifetime;  // xyz = position, w = lifetime
    glm::vec4 velAndMaxLife;   // xyz = velocity, w = maxLifetime
    glm::vec4 color;           // rgba
};

struct ParticleVertex {
    glm::vec3 position;
    float     _pad;      // padding to match std430 vec4 alignment
    glm::vec4 color;
};

class ParticleSystem {
public:
    ParticleSystem(const Device& device, VkRenderPass renderPass,
                   uint32_t maxParticles = 512);
    ~ParticleSystem();

    ParticleSystem(const ParticleSystem&)            = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    void setEmitter(glm::vec3 pos, glm::vec3 baseColor, float emitRate = 60.0f);
    void update(float dt);
    void dispatchCompute(VkCommandBuffer cmd, float dt);
    void record(VkCommandBuffer cmd, const glm::mat4& viewProj, float particleSize = 40.0f);

    uint32_t getAliveCount() const { return m_aliveCount; }

private:
    const Device&  m_device;
    VmaAllocator   m_allocator;

    uint32_t m_maxParticles;
    uint32_t m_aliveCount = 0;
    float    m_emitAccum  = 0.0f;

    // Emitter config
    glm::vec3 m_emitterPos   = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 m_baseColor    = glm::vec3(1.0f, 0.6f, 0.2f);
    float     m_emitRate     = 60.0f;

    // GPU resources: compute SSBO for particle data
    VkBuffer      m_particleBuffer    = VK_NULL_HANDLE;
    VmaAllocation m_particleAlloc     = VK_NULL_HANDLE;
    void*         m_particleMapped    = nullptr;

    // GPU resources: compute output vertex buffer
    VkBuffer      m_vertexBuffer    = VK_NULL_HANDLE;
    VmaAllocation m_vertexAlloc     = VK_NULL_HANDLE;

    // GPU resources: atomic counter buffer
    VkBuffer      m_counterBuffer   = VK_NULL_HANDLE;
    VmaAllocation m_counterAlloc    = VK_NULL_HANDLE;
    void*         m_counterMapped   = nullptr;

    // Compute pipeline
    VkDescriptorSetLayout m_computeDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_computeDescPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_computeDescSet    = VK_NULL_HANDLE;
    VkPipelineLayout      m_computePipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_computePipeline   = VK_NULL_HANDLE;

    // Graphics pipeline
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;

    std::mt19937 m_rng{42};

    void createComputePipeline();
    void createGraphicsPipeline(VkRenderPass renderPass);
    void emit(uint32_t count);
    float randFloat(float lo, float hi);
};

} // namespace glory
