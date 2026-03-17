#include "combat/RespawnSystem.h"
#include "combat/EconomySystem.h"
#include "audio/GameAudioEvents.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <vector>

namespace glory {

// ── Main update ─────────────────────────────────────────────────────────────
void RespawnSystem::update(entt::registry& reg, float dt) {
    // Collect entities to process (avoid invalidation during iteration)
    std::vector<entt::entity> toDestroy;

    auto view = reg.view<RespawnComponent, StatsComponent>();
    for (auto [entity, rc, stats] : view.each()) {
        switch (rc.state) {
        case LifeState::ALIVE:
            if (stats.base.currentHP <= 0.0f) {
                handleDeath(reg, entity, rc);
            }
            break;

        case LifeState::DYING:
            // Brief death animation period (~1s) before entering DEAD
            rc.respawnTimer -= dt;
            if (rc.respawnTimer <= 0.0f) {
                if (!rc.isHero) {
                    // Minions: destroy entity
                    toDestroy.push_back(entity);
                } else {
                    // Hero: transition to DEAD with full respawn timer
                    int level = 1;
                    if (auto* eco = reg.try_get<EconomyComponent>(entity))
                        level = eco->level;
                    float respawnTime = 10.0f + static_cast<float>(level) * 2.0f;
                    rc.state        = LifeState::DEAD;
                    rc.respawnTimer = respawnTime;
                    rc.totalRespawn = respawnTime;
                    spdlog::info("Hero entering respawn timer: {:.1f}s", respawnTime);
                }
            }
            break;

        case LifeState::DEAD:
            rc.respawnTimer -= dt;
            if (rc.respawnTimer <= 0.0f) {
                handleRespawn(reg, entity, rc);
            }
            break;
        }
    }

    // Destroy dead minions
    for (auto e : toDestroy) {
        if (reg.valid(e)) reg.destroy(e);
    }
}

// ── Handle death transition ─────────────────────────────────────────────────
void RespawnSystem::handleDeath(entt::registry& reg, entt::entity e,
                                 RespawnComponent& rc) {
    // Record death position
    if (reg.all_of<TransformComponent>(e))
        rc.deathPosition = reg.get<TransformComponent>(e).position;

    // Play death animation (clip index 3 if available, else freeze on idle)
    if (reg.all_of<AnimationComponent>(e)) {
        auto& anim = reg.get<AnimationComponent>(e);
        int deathClip = -1;
        for (int i = 0; i < static_cast<int>(anim.clips.size()); ++i) {
            if (anim.clips[i].name == "death" || anim.clips[i].name == "Death" ||
                anim.clips[i].name == "die"   || anim.clips[i].name == "Die") {
                deathClip = i;
                break;
            }
        }
        if (deathClip < 0 && anim.clips.size() > 3)
            deathClip = 3; // fallback: 4th clip
        if (deathClip >= 0) {
            anim.activeClipIndex = deathClip;
            anim.player.crossfadeTo(&anim.clips[deathClip], 0.15f);
            anim.player.setTimeScale(1.0f);
        }
    }

    // Audio death event
    if (m_audio) {
        m_audio->onDeath(rc.deathPosition);
    }

    // Semi-transparent rendering
    auto& tint = reg.get_or_emplace<TintComponent>(e);
    tint.color.a = 0.3f;

    // Stop movement
    if (reg.all_of<UnitComponent>(e)) {
        auto& unit = reg.get<UnitComponent>(e);
        unit.state = UnitComponent::State::IDLE;
    }
    if (reg.all_of<CharacterComponent>(e)) {
        auto& cc = reg.get<CharacterComponent>(e);
        cc.hasTarget = false;
    }

    // Set combat state to prevent attacks
    if (reg.all_of<CombatComponent>(e)) {
        auto& combat = reg.get<CombatComponent>(e);
        combat.state = CombatState::STUNNED;
        combat.stateTimer = 999.0f; // effectively locked
    }

    // DYING state: brief animation window before respawn timer starts
    rc.state = LifeState::DYING;
    rc.respawnTimer = rc.isHero ? 1.5f : 1.0f; // death animation duration

    spdlog::info("Entity died at ({:.0f}, {:.0f})", rc.deathPosition.x, rc.deathPosition.z);
}

// ── Handle respawn ──────────────────────────────────────────────────────────
void RespawnSystem::handleRespawn(entt::registry& reg, entt::entity e,
                                   RespawnComponent& rc) {
    // Determine fountain position from team
    uint8_t teamIdx = 0;
    if (auto* tc = reg.try_get<TeamComponent>(e))
        teamIdx = static_cast<uint8_t>(tc->team);
    glm::vec3 fountain = getFountainPosition(teamIdx);

    // Teleport to fountain
    if (reg.all_of<TransformComponent>(e))
        reg.get<TransformComponent>(e).position = fountain;

    // Restore HP/MP
    if (reg.all_of<StatsComponent>(e)) {
        auto& stats = reg.get<StatsComponent>(e);
        stats.base.currentHP = stats.total().maxHP;
    }
    if (reg.all_of<ResourceComponent>(e)) {
        auto& res = reg.get<ResourceComponent>(e);
        res.current = res.maximum;
    }

    // Restore opacity
    if (auto* tint = reg.try_get<TintComponent>(e))
        tint->color.a = 1.0f;

    // Unlock combat
    if (reg.all_of<CombatComponent>(e)) {
        auto& combat = reg.get<CombatComponent>(e);
        combat.state = CombatState::IDLE;
        combat.stateTimer = 0.0f;
    }

    // Reset animation to idle
    if (reg.all_of<AnimationComponent>(e)) {
        auto& anim = reg.get<AnimationComponent>(e);
        if (!anim.clips.empty()) {
            anim.activeClipIndex = 0;
            anim.player.crossfadeTo(&anim.clips[0], 0.2f);
        }
    }

    rc.state = LifeState::ALIVE;
    rc.respawnTimer = 0.0f;

    spdlog::info("Hero respawned at fountain ({:.0f}, {:.0f})", fountain.x, fountain.z);

    if (onRespawn) onRespawn(e, fountain);
}

// ── Get fountain position from map data ─────────────────────────────────────
glm::vec3 RespawnSystem::getFountainPosition(uint8_t teamIndex) const {
    if (m_mapData && teamIndex < 2) {
        // Fountain is near the nexus, offset slightly behind
        glm::vec3 nexus = m_mapData->teams[teamIndex].base.nexusPosition;
        // Offset toward the map corner (behind nexus)
        glm::vec3 center(100.0f, 0.0f, 100.0f);
        glm::vec3 dir = nexus - center;
        if (glm::dot(dir, dir) > 0.01f)
            dir = glm::normalize(dir);
        return nexus + dir * 8.0f; // 8 units behind nexus
    }
    // Fallback: team 0 blue corner, team 1 red corner
    return (teamIndex == 0) ? glm::vec3(15.f, 0.f, 15.f) : glm::vec3(185.f, 0.f, 185.f);
}

} // namespace glory
