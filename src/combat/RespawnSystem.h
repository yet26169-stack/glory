#pragma once
/// RespawnSystem: Hero death/respawn lifecycle, minion cleanup, kill awards.
///
/// When a hero's HP ≤ 0:
///   1. Play death animation, set LifeState::DEAD
///   2. Disable movement/input (checked in GameplaySystem)
///   3. Start respawn timer: 10 + level × 2 seconds
///   4. Render semi-transparent (TintComponent alpha = 0.3)
///   5. On timer expiry: teleport to fountain, restore HP/MP, set ALIVE
///
/// Minions that die are destroyed outright (entity removed).

#include "combat/CombatComponents.h"
#include "ability/AbilityComponents.h"
#include "combat/MinionWaveSystem.h" // TintComponent
#include "scene/Components.h"
#include "map/MapTypes.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <functional>

namespace glory {

class EconomySystem;
class GameAudioEvents;

// ── Life state ──────────────────────────────────────────────────────────────
enum class LifeState : uint8_t { ALIVE, DYING, DEAD };

struct RespawnComponent {
    LifeState state          = LifeState::ALIVE;
    float     respawnTimer   = 0.0f;   // seconds remaining
    float     totalRespawn   = 0.0f;   // total respawn time (for HUD fraction)
    glm::vec3 deathPosition  {0.f};    // where the hero died (camera stays here)
    bool      isHero         = true;   // false for minions (destroy instead)
};

// ── Respawn System ──────────────────────────────────────────────────────────
class RespawnSystem {
public:
    void init(const MapData& mapData, EconomySystem* economy,
              GameAudioEvents* audio) {
        m_mapData  = &mapData;
        m_economy  = economy;
        m_audio    = audio;
    }

    /// Main tick — call after all damage-dealing systems.
    void update(entt::registry& reg, float dt);

    /// Check if the local player hero is dead (for camera/HUD gating).
    static bool isDead(const entt::registry& reg, entt::entity e) {
        if (e == entt::null) return false;
        auto* rc = reg.try_get<RespawnComponent>(e);
        return rc && rc->state != LifeState::ALIVE;
    }

    /// Callback when any hero respawns (entity, fountain position).
    std::function<void(entt::entity, glm::vec3)> onRespawn;

private:
    const MapData*    m_mapData  = nullptr;
    EconomySystem*    m_economy  = nullptr;
    GameAudioEvents*  m_audio    = nullptr;

    void handleDeath(entt::registry& reg, entt::entity e, RespawnComponent& rc);
    void handleRespawn(entt::registry& reg, entt::entity e, RespawnComponent& rc);
    glm::vec3 getFountainPosition(uint8_t teamIndex) const;
};

} // namespace glory
