#pragma once

// ── Structure System: Towers, Inhibitors, Nexus ─────────────────────────
// StructureComponent: per-entity structure data (type, lane, gating)
// StructureSystem:    auto-attack AI, inhibitor respawn, nexus death

#include "map/MapTypes.h"
#include "combat/CombatComponents.h"
#include "ability/AbilityComponents.h"
#include "scene/Components.h"

#include "vfx/VFXEventQueue.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <functional>
#include <array>
#include <vector>
#include <cstdint>

namespace glory {

class EconomySystem;

// ── Structure types ──────────────────────────────────────────────────────

enum class StructureType : uint8_t {
    TOWER_T1,       // Outer tower
    TOWER_T2,       // Inner tower
    TOWER_T3,       // Inhibitor tower
    TOWER_NEXUS,    // Nexus tower
    INHIBITOR,
    NEXUS,
};

// ── StructureComponent ───────────────────────────────────────────────────

struct StructureComponent {
    StructureType type       = StructureType::TOWER_T1;
    uint8_t       teamIndex  = 0;          // 0 = Blue, 1 = Red
    LaneType      lane       = LaneType::Mid;
    bool          isDestroyed = false;

    // Auto-attack state
    float         attackDamage    = 150.0f;
    float         attackRange     = 15.0f;
    float         attackSpeed     = 1.2f;  // attacks per second
    float         attackCooldown  = 0.0f;
    float         projectileSpeed = 30.0f;
    entt::entity  currentTarget   = entt::null;

    // Targeting priority: if an enemy hero attacks an allied hero, switch target
    entt::entity  aggroOverride   = entt::null;
    float         aggroTimer      = 0.0f;

    // Inhibitor respawn
    float         respawnTimer    = 0.0f;
    float         respawnTime     = 300.0f; // 5 minutes

    // Lane gating: prerequisite structure entity (must be destroyed first)
    entt::entity  prerequisite    = entt::null;
};

// ── StructureSystem ──────────────────────────────────────────────────────

class StructureSystem {
public:
    using VictoryCallback = std::function<void(uint8_t winningTeam)>;

    void setVictoryCallback(VictoryCallback cb) { m_onVictory = std::move(cb); }
    void setEconomySystem(EconomySystem* econ) { m_economy = econ; }
    void setVFXQueue(VFXEventQueue* q) { m_vfxQueue = q; }

    void update(entt::registry& reg, float dt);

    // Spawn all structures from MapData. Returns entities for gating linkage.
    struct SpawnResult {
        // [teamIndex][laneIndex] = array of tower entities per tier (T1, T2, T3)
        std::array<std::array<std::array<entt::entity, 3>, 3>, 2> laneTowers;
        // [teamIndex][laneIndex] = inhibitor entity
        std::array<std::array<entt::entity, 3>, 2> inhibitors;
        // [teamIndex] = nexus entity
        std::array<entt::entity, 2> nexuses;
        // [teamIndex] = nexus tower entities
        std::array<std::vector<entt::entity>, 2> nexusTowers;
    };

    SpawnResult spawnStructures(entt::registry& reg, const MapData& mapData);

private:
    VictoryCallback m_onVictory;
    EconomySystem*  m_economy  = nullptr;
    VFXEventQueue*  m_vfxQueue = nullptr;

    void updateTowerAI(entt::registry& reg, entt::entity entity,
                       StructureComponent& sc, float dt);
    void updateInhibitorRespawn(entt::registry& reg, entt::entity entity,
                                StructureComponent& sc, float dt);
    void applyTowerHit(entt::registry& reg, entt::entity tower,
                       entt::entity target, float damage);
    entt::entity findBestTarget(entt::registry& reg, entt::entity tower,
                                const StructureComponent& sc,
                                const TransformComponent& tc);
    bool isPrerequisiteMet(const entt::registry& reg, const StructureComponent& sc) const;
};

} // namespace glory
