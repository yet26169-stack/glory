#pragma once

#include "ability/AbilityComponents.h"
#include "vfx/VFXEventQueue.h"
#include "vfx/VFXTypes.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace glory {

class AbilitySystem;
class TrailRenderer;

// Moves projectile entities each frame, syncs their trail VFX, checks collisions,
// and resolves hits via AbilitySystem when they connect or expire.
class ProjectileSystem {
public:
    void update(entt::registry& reg, float dt,
                VFXEventQueue& vfxQueue, AbilitySystem& abilitySystem,
                TrailRenderer* trailRenderer = nullptr);

    // Positions of lob projectiles that landed this frame — read by Renderer each frame.
    const std::vector<glm::vec3>& getLandedPositions() const { return m_landedPositions; }
    void clearLandedPositions() { m_landedPositions.clear(); }

    // Registers a pre-loaded mesh so projectiles with this abilityID get a 3D model.
    struct ProjectileMeshInfo {
        uint32_t meshIndex  = UINT32_MAX;
        uint32_t texIndex   = 0;
        uint32_t normalIndex = 0;
        glm::vec3 scale     = {1.f, 1.f, 1.f};
    };
    void registerAbilityMesh(const std::string& abilityID, ProjectileMeshInfo info);

private:
    std::unordered_map<std::string, ProjectileMeshInfo> m_abilityMeshes;
    std::vector<glm::vec3> m_landedPositions;  // filled each frame by lob landing events
    void destroyProjectile(entt::registry& reg, entt::entity e,
                           const ProjectileComponent& pc,
                           VFXEventQueue& vfxQueue,
                           AbilitySystem* abilitySystem,
                           const glm::vec3& hitPos,
                           bool applyHit,
                           TrailRenderer* trailRenderer = nullptr);
};

} // namespace glory
