#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace glory {

struct MaterialProperties {
    glm::vec3 albedo{1.0f};
    float     metallic  = 0.0f;
    float     roughness = 0.5f;
    float     ao        = 1.0f; // ambient occlusion
};

// Lightweight material — no Vulkan resources.
// Texture lookups use bindless indices stored in textureIndices[].
class Material {
public:
    Material() = default;

    MaterialProperties& getProperties() { return m_props; }
    const MaterialProperties& getProperties() const { return m_props; }

    // Bindless texture slots (returned by BindlessDescriptors::registerTexture)
    // 0=albedo, 1=normal, 2=metallic-roughness, 3=emissive, 4-7=reserved
    uint32_t textureIndices[8] = {};

private:
    MaterialProperties m_props;
};

} // namespace glory
