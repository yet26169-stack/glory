#pragma once
/// FogOfWarGameplay: team-based vision gathering, entity visibility tracking,
/// ward lifecycle, and fade-in/fade-out logic for MOBA fog of war.

#include "fog/FogComponents.h"
#include "fog/FogSystem.h"
#include "combat/CombatComponents.h"
#include "scene/Components.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <vector>

namespace glory {

class FogOfWarGameplay {
public:
    static constexpr float FADE_IN_TIME  = 0.2f;   // seconds to fully appear
    static constexpr float FADE_OUT_TIME = 1.0f;   // seconds ghost is visible
    static constexpr float WARD_SIGHT    = 8.0f;
    static constexpr float WARD_DURATION = 180.0f;

    // Vision radii defaults (used if no VisionComponent attached)
    static constexpr float HERO_VISION   = 12.0f;
    static constexpr float MINION_VISION = 6.0f;
    static constexpr float TOWER_VISION  = 8.0f;

    /// Main tick. Call before FogSystem::update().
    /// 1. Gathers allied vision sources → fills visionEntities for FogSystem
    /// 2. Queries FogSystem visibility for each enemy entity
    /// 3. Updates FowVisibilityComponent fade states
    /// 4. Ticks ward lifetimes
    void update(entt::registry& reg, FogSystem& fogSystem, float dt,
                Team localTeam);

    /// Place a ward at the given world position for the given team.
    entt::entity placeWard(entt::registry& reg, const glm::vec3& position,
                           Team team);

    /// Check if an entity should be rendered (for draw-loop filtering).
    static bool shouldRender(const entt::registry& reg, entt::entity e,
                             Team localTeam);

    /// Get the render alpha for an entity (for ghost/fade rendering).
    static float getRenderAlpha(const entt::registry& reg, entt::entity e);

    /// Check visibility on minimap (visible or fading out = show dot).
    static bool isVisibleOnMinimap(const entt::registry& reg, entt::entity e,
                                   Team localTeam);

private:
    std::vector<VisionEntity> m_visionEntities;

    void gatherVisionSources(entt::registry& reg, Team localTeam);
    void updateEnemyVisibility(entt::registry& reg, FogSystem& fogSystem,
                               float dt, Team localTeam);
    void tickWards(entt::registry& reg, float dt);
};

} // namespace glory
