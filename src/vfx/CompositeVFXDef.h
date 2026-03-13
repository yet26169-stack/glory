#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <cstdint>

namespace glory {

enum class VFXLayerType : uint8_t {
    PARTICLE,       // → VFXRenderer
    TRAIL,          // → TrailRenderer
    GROUND_DECAL,   // → GroundDecalRenderer
    MESH_EFFECT,    // → MeshEffectRenderer
    SHOCKWAVE,      // → ExplosionRenderer
    CONE,           // → ConeAbilityRenderer
    SHIELD,         // → ShieldBubbleRenderer
    SPRITE_EFFECT,  // → SpriteEffectRenderer
    DISTORTION,     // → DistortionRenderer
};

struct VFXLayer {
    float          delay      = 0.0f;    // seconds after trigger to fire
    VFXLayerType   type       = VFXLayerType::PARTICLE;
    std::string    effectRef;             // ID in the relevant subsystem
    float          duration   = -1.0f;   // override duration (-1 = use default)
    float          scale      = 1.0f;
    glm::vec4      color      = {1,1,1,1};

    enum class Anchor : uint8_t { CASTER, TARGET, WORLD };
    Anchor         anchor     = Anchor::CASTER;
    glm::vec3      offset     = {0,0,0};
};

struct CompositeVFXDef {
    std::string            id;
    std::vector<VFXLayer>  layers;
};

} // namespace glory
