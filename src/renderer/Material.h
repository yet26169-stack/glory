#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <string>

namespace glory {

class Device;
class Texture;

struct MaterialProperties {
    glm::vec3 albedo{1.0f};
    float     metallic  = 0.0f;
    float     roughness = 0.5f;
    float     ao        = 1.0f; // ambient occlusion
};

class Material {
public:
    Material() = default;
    Material(const Device& device, VkDescriptorSetLayout layout,
             VkDescriptorPool pool, uint32_t frameCount);
    ~Material();

    Material(const Material&)            = delete;
    Material& operator=(const Material&) = delete;
    Material(Material&& other) noexcept;
    Material& operator=(Material&& other) noexcept;

    void setAlbedoTexture(const Texture& tex);
    void bind(VkCommandBuffer cmd, VkPipelineLayout pipelineLayout,
              uint32_t frameIndex) const;

    MaterialProperties& getProperties() { return m_props; }
    const MaterialProperties& getProperties() const { return m_props; }

    VkDescriptorSet getDescriptorSet(uint32_t frameIndex) const {
        return m_descriptorSets[frameIndex];
    }

private:
    MaterialProperties m_props;
    std::vector<VkDescriptorSet> m_descriptorSets;
    VkDevice m_vkDevice = VK_NULL_HANDLE;
};

} // namespace glory
