#pragma once

#include "vfx/CompositeVFXDef.h"
#include "vfx/VFXEventQueue.h"
#include <unordered_map>
#include <memory>

namespace glory {

class TrailRenderer;
class GroundDecalRenderer;
class MeshEffectRenderer;
class ExplosionRenderer;
class ConeAbilityRenderer;
class SpriteEffectRenderer;
class DistortionRenderer;

class CompositeVFXSequencer {
public:
    void loadDirectory(const std::string& dirPath);

    uint32_t trigger(const std::string& compositeId,
                     glm::vec3 casterPos,
                     glm::vec3 targetPos,
                     glm::vec3 direction);

    void cancel(uint32_t handle);

    void update(float dt,
                VFXEventQueue& particleQueue,
                TrailRenderer& trails,
                GroundDecalRenderer& decals,
                MeshEffectRenderer& meshFX,
                ExplosionRenderer& explosions,
                ConeAbilityRenderer& cones,
                SpriteEffectRenderer& sprites,
                DistortionRenderer& distortions);

private:
    struct ActiveComposite {
        uint32_t     handle;
        const CompositeVFXDef* def;
        float        elapsed = 0.0f;
        glm::vec3    casterPos, targetPos, direction;
        std::vector<bool> fired;
    };

    std::unordered_map<std::string, CompositeVFXDef> m_defs;
    std::vector<ActiveComposite> m_active;
    uint32_t m_nextHandle = 1;
};

} // namespace glory
