#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <vector>

namespace glory {

class Device;

// binding 0 — vertex stage (per-frame shared data; per-entity model/normal in instance buffer)
struct UniformBufferObject {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
    alignas(16) glm::mat4 lightSpaceMatrix;
};

// binding 2 — fragment stage
struct PointLightData {
    alignas(16) glm::vec3 position;
    alignas(16) glm::vec3 color;
};

static constexpr uint32_t MAX_LIGHTS = 4;

struct LightUBO {
    PointLightData lights[MAX_LIGHTS];
    alignas(16) glm::vec3 viewPos;
    alignas(4)  int       lightCount       = 0;
    alignas(4)  float     ambientStrength  = 0.15f;
    alignas(4)  float     specularStrength = 0.5f;
    alignas(4)  float     shininess        = 32.0f;
    alignas(16) glm::vec3 fogColor{0.6f, 0.65f, 0.75f};
    alignas(4)  float     fogDensity       = 0.03f;
    alignas(4)  float     fogStart         = 5.0f;
    alignas(4)  float     fogEnd           = 50.0f;
};

class Descriptors {
public:
    static constexpr uint32_t MAX_BINDLESS_TEXTURES = 64;

    Descriptors(const Device& device, uint32_t frameCount);
    ~Descriptors();

    Descriptors(const Descriptors&)            = delete;
    Descriptors& operator=(const Descriptors&) = delete;

    void cleanup();

    void updateUniformBuffer(uint32_t frameIndex, const UniformBufferObject& ubo);
    void updateLightBuffer(uint32_t frameIndex, const LightUBO& light);
    void writeBindlessTexture(uint32_t arrayIndex, VkImageView imageView, VkSampler sampler);
    void updateShadowMap(VkImageView depthView, VkSampler shadowSampler);

    VkDescriptorSetLayout getLayout() const { return m_layout; }
    VkDescriptorPool      getPool()   const { return m_pool; }
    VkDescriptorSet       getSet(uint32_t frameIndex) const { return m_sets[frameIndex]; }

private:
    const Device& m_device;
    uint32_t m_frameCount = 0;

    VkDescriptorSetLayout        m_layout = VK_NULL_HANDLE;
    VkDescriptorPool             m_pool   = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_sets;

    // Per-frame uniform buffers (host-visible, persistently mapped)
    std::vector<VkBuffer>      m_uniformBuffers;
    std::vector<VmaAllocation> m_uniformAllocations;
    std::vector<void*>         m_uniformMapped;

    // Per-frame light UBOs
    std::vector<VkBuffer>      m_lightBuffers;
    std::vector<VmaAllocation> m_lightAllocations;
    std::vector<void*>         m_lightMapped;

    bool m_cleaned = false;

    void createLayout();
    void createUniformBuffers(uint32_t frameCount);
    void createPool(uint32_t frameCount);
    void createSets(uint32_t frameCount);
};

} // namespace glory
