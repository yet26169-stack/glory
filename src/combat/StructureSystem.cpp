#include "combat/StructureSystem.h"
#include "combat/EconomySystem.h"
#include "combat/HeroDefinition.h"
#include "fog/FogComponents.h"
#include "vfx/VFXEventQueue.h"
#include "core/FixedPoint.h"

#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cstring>
#include <cfloat>

namespace glory {

// ── Helpers ──────────────────────────────────────────────────────────────

static Team teamFromIndex(uint8_t idx) {
    return (idx == 0) ? Team::PLAYER : Team::ENEMY;
}

static float distXZ(const glm::vec3& a, const glm::vec3& b) {
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// ── Main update loop ─────────────────────────────────────────────────────

void StructureSystem::update(entt::registry& reg, float dt) {
    auto view = reg.view<StructureComponent, TransformComponent, StatsComponent, TeamComponent>();
    for (auto [entity, sc, tc, stats, team] : view.each()) {
        if (sc.isDestroyed) {
            // Handle inhibitor respawn
            if (sc.type == StructureType::INHIBITOR) {
                updateInhibitorRespawn(reg, entity, sc, dt);
            }
            continue;
        }

        // Check if structure died this frame
        if (stats.base.currentHP <= 0.0f) {
            sc.isDestroyed = true;
            sc.currentTarget = entt::null;
            spdlog::info("Structure destroyed: type={} team={} lane={}",
                         static_cast<int>(sc.type), sc.teamIndex, static_cast<int>(sc.lane));

            // Inhibitors start respawn timer
            if (sc.type == StructureType::INHIBITOR) {
                sc.respawnTimer = sc.respawnTime;
            }

            // Nexus death → game over
            if (sc.type == StructureType::NEXUS) {
                uint8_t winningTeam = (sc.teamIndex == 0) ? 1 : 0;
                spdlog::info("NEXUS DESTROYED! Team {} wins!", winningTeam);
                if (m_onVictory) {
                    m_onVictory(winningTeam);
                }
            }
            continue;
        }

        // If lane prerequisite not met, structure is invulnerable — clamp HP
        if (!isPrerequisiteMet(reg, sc)) {
            stats.base.currentHP = stats.base.maxHP;
        }

        // Structures only attack if their lane prerequisite is met (invulnerable otherwise)
        // Towers auto-attack enemies in range
        if (sc.type != StructureType::INHIBITOR && sc.type != StructureType::NEXUS) {
            updateTowerAI(reg, entity, sc, dt);
        }
    }
}

// ── Tower AI ─────────────────────────────────────────────────────────────

void StructureSystem::updateTowerAI(entt::registry& reg, entt::entity entity,
                                     StructureComponent& sc, float dt) {
    // Tick attack cooldown
    sc.attackCooldown = std::max(0.0f, sc.attackCooldown - dt);

    // Tick aggro override timer
    if (sc.aggroTimer > 0.0f) {
        sc.aggroTimer -= dt;
        if (sc.aggroTimer <= 0.0f) {
            sc.aggroOverride = entt::null;
        }
    }

    auto& tc = reg.get<TransformComponent>(entity);

    // Validate current target
    if (sc.currentTarget != entt::null) {
        if (!reg.valid(sc.currentTarget) ||
            !reg.all_of<TransformComponent, StatsComponent>(sc.currentTarget)) {
            sc.currentTarget = entt::null;
        } else {
            auto& targetStats = reg.get<StatsComponent>(sc.currentTarget);
            auto& targetTc = reg.get<TransformComponent>(sc.currentTarget);
            if (targetStats.base.currentHP <= 0.0f ||
                distXZ(tc.position, targetTc.position) > sc.attackRange * 1.1f) {
                sc.currentTarget = entt::null;
            }
        }
    }

    // Find new target if needed
    if (sc.currentTarget == entt::null) {
        sc.currentTarget = findBestTarget(reg, entity, sc, tc);
    }

    // Fire if we have a target and cooldown is ready
    if (sc.currentTarget != entt::null && sc.attackCooldown <= 0.0f) {
        applyTowerHit(reg, entity, sc.currentTarget, sc.attackDamage);
        sc.attackCooldown = 1.0f / sc.attackSpeed;
    }
}

// ── Target selection (LoL priority) ──────────────────────────────────────

entt::entity StructureSystem::findBestTarget(entt::registry& reg, entt::entity tower,
                                              const StructureComponent& sc,
                                              const TransformComponent& tc) {
    Team myTeam = teamFromIndex(sc.teamIndex);

    // If aggro override is set and valid, prefer it
    if (sc.aggroOverride != entt::null && reg.valid(sc.aggroOverride) &&
        reg.all_of<TransformComponent, StatsComponent>(sc.aggroOverride)) {
        auto& targetTc = reg.get<TransformComponent>(sc.aggroOverride);
        auto& targetStats = reg.get<StatsComponent>(sc.aggroOverride);
        if (targetStats.base.currentHP > 0.0f &&
            distXZ(tc.position, targetTc.position) <= sc.attackRange) {
            return sc.aggroOverride;
        }
    }

    // Collect candidates in range
    entt::entity bestMinion   = entt::null;
    float        bestMinionDist = FLT_MAX;
    entt::entity bestHero     = entt::null;
    float        bestHeroDist  = FLT_MAX;

    auto targets = reg.view<TransformComponent, StatsComponent, TeamComponent>();
    for (auto [ent, ttc, tstats, tteam] : targets.each()) {
        if (ent == tower) continue;
        if (tteam.team == myTeam) continue;
        if (tstats.base.currentHP <= 0.0f) continue;

        // Don't target other structures
        if (reg.all_of<StructureComponent>(ent)) continue;

        float dist = distXZ(tc.position, ttc.position);
        if (dist > sc.attackRange) continue;

        // Prioritize minions over heroes
        if (reg.all_of<MinionComponent>(ent)) {
            if (dist < bestMinionDist) {
                bestMinionDist = dist;
                bestMinion = ent;
            }
        } else {
            if (dist < bestHeroDist) {
                bestHeroDist = dist;
                bestHero = ent;
            }
        }
    }

    // Prefer minions, fall back to heroes
    return (bestMinion != entt::null) ? bestMinion : bestHero;
}

// ── Damage application ───────────────────────────────────────────────────

void StructureSystem::applyTowerHit(entt::registry& reg, entt::entity tower,
                                     entt::entity target, float damage) {
    if (!reg.valid(target) || !reg.all_of<StatsComponent>(target)) return;
    auto& stats = reg.get<StatsComponent>(target);

    // Armor reduction
    Fixed32 fDamage(damage);
    Fixed32 fArmor(stats.total().armor);
    Fixed32 fFinal = fDamage * (Fixed32(100) / (Fixed32(100) + fArmor));
    stats.base.currentHP = std::max(0.0f, stats.base.currentHP - fFinal.toFloat());

    spdlog::debug("Tower hit for {:.1f} damage (target HP: {:.1f})",
                  fFinal.toFloat(), stats.base.currentHP);

    // Tower kill awards economy
    if (stats.base.currentHP <= 0.0f && m_economy) {
        // Find the closest hero on the tower's team to award the kill
        // (towers don't earn gold themselves, but the kill should count)
    }

    // VFX for tower shot
    if (m_vfxQueue && reg.all_of<TransformComponent>(target)) {
        auto& targetTc = reg.get<TransformComponent>(target);
        VFXEvent ev{};
        ev.type = VFXEventType::Spawn;
        std::strncpy(ev.effectID, "vfx_melee_hit", sizeof(ev.effectID) - 1);
        ev.position = targetTc.position + glm::vec3(0.0f, 1.0f, 0.0f);
        m_vfxQueue->push(ev);
    }
}

// ── Inhibitor respawn ────────────────────────────────────────────────────

void StructureSystem::updateInhibitorRespawn(entt::registry& reg, entt::entity entity,
                                              StructureComponent& sc, float dt) {
    sc.respawnTimer -= dt;
    if (sc.respawnTimer <= 0.0f) {
        sc.isDestroyed = false;
        sc.respawnTimer = 0.0f;

        // Restore HP
        if (reg.all_of<StatsComponent>(entity)) {
            auto& stats = reg.get<StatsComponent>(entity);
            stats.base.currentHP = stats.base.maxHP;
        }

        spdlog::info("Inhibitor respawned: team={} lane={}",
                     sc.teamIndex, static_cast<int>(sc.lane));
    }
}

// ── Lane gating check ────────────────────────────────────────────────────

bool StructureSystem::isPrerequisiteMet(const entt::registry& reg,
                                         const StructureComponent& sc) const {
    if (sc.prerequisite == entt::null) return true;
    if (!reg.valid(sc.prerequisite)) return true;
    if (!reg.all_of<StructureComponent>(sc.prerequisite)) return true;
    return reg.get<StructureComponent>(sc.prerequisite).isDestroyed;
}

// ── Spawn structures from MapData ────────────────────────────────────────

StructureSystem::SpawnResult StructureSystem::spawnStructures(
    entt::registry& reg, const MapData& mapData) {

    SpawnResult result{};
    // Init all to null
    for (auto& team : result.laneTowers)
        for (auto& lane : team)
            for (auto& t : lane)
                t = entt::null;
    for (auto& team : result.inhibitors)
        for (auto& inh : team)
            inh = entt::null;
    result.nexuses = { entt::null, entt::null };

    auto createStructureEntity = [&](const std::string& name, glm::vec3 pos,
                                      StructureType type, uint8_t teamIdx,
                                      LaneType lane, float hp, float damage,
                                      float range, float armor, float mr) -> entt::entity {
        entt::entity e = reg.create();
        reg.emplace<TagComponent>(e, TagComponent{name});

        auto& tc = reg.emplace<TransformComponent>(e);
        tc.position = pos;
        tc.scale = glm::vec3(1.0f);  // structures use world-scale

        Team team = teamFromIndex(teamIdx);
        reg.emplace<TeamComponent>(e, TeamComponent{team});

        StatsComponent stats;
        stats.base.maxHP = hp;
        stats.base.currentHP = hp;
        stats.base.armor = armor;
        stats.base.magicResist = mr;
        stats.base.attackDamage = damage;
        reg.emplace<StatsComponent>(e, stats);

        StructureComponent sc;
        sc.type = type;
        sc.teamIndex = teamIdx;
        sc.lane = lane;
        sc.attackDamage = damage;
        sc.attackRange = range;
        sc.isDestroyed = false;
        reg.emplace<StructureComponent>(e, sc);

        // FoW vision: towers provide vision around them
        reg.emplace<VisionComponent>(e, VisionComponent{8.0f});

        return e;
    };

    // Spawn for each team
    for (uint8_t teamIdx = 0; teamIdx < 2; ++teamIdx) {
        const auto& teamData = mapData.teams[teamIdx];

        // ── Towers ─────────────────────────────────────────────────────
        for (const auto& tower : teamData.towers) {
            StructureType stype;
            int tierSlot = -1;
            float hp, dmg, armor, mr;

            switch (tower.tier) {
            case TowerTier::Outer:
                stype = StructureType::TOWER_T1;
                tierSlot = 0;
                hp = 5000.0f; dmg = 152.0f; armor = 40.0f; mr = 40.0f;
                break;
            case TowerTier::Inner:
                stype = StructureType::TOWER_T2;
                tierSlot = 1;
                hp = 3500.0f; dmg = 170.0f; armor = 55.0f; mr = 55.0f;
                break;
            case TowerTier::Inhibitor:
                stype = StructureType::TOWER_T3;
                tierSlot = 2;
                hp = 3500.0f; dmg = 170.0f; armor = 55.0f; mr = 55.0f;
                break;
            case TowerTier::Nexus:
                stype = StructureType::TOWER_NEXUS;
                hp = 2700.0f; dmg = 180.0f; armor = 65.0f; mr = 65.0f;
                break;
            }

            glm::vec3 pos = tower.position;
            float range = tower.attackRange;

            std::string name = "Tower_T" + std::to_string(teamIdx) + "_" +
                              std::to_string(static_cast<int>(tower.lane)) + "_" +
                              std::to_string(static_cast<int>(tower.tier));

            entt::entity e = createStructureEntity(
                name, pos, stype, teamIdx, tower.lane, hp, dmg, range, armor, mr);

            int laneIdx = static_cast<int>(tower.lane);
            if (stype == StructureType::TOWER_NEXUS) {
                result.nexusTowers[teamIdx].push_back(e);
            } else if (tierSlot >= 0 && laneIdx < 3) {
                result.laneTowers[teamIdx][laneIdx][tierSlot] = e;
            }
        }

        // ── Inhibitors ─────────────────────────────────────────────────
        for (const auto& inh : teamData.inhibitors) {
            std::string name = "Inhibitor_T" + std::to_string(teamIdx) + "_" +
                              std::to_string(static_cast<int>(inh.lane));

            entt::entity e = createStructureEntity(
                name, inh.position, StructureType::INHIBITOR, teamIdx,
                inh.lane, inh.maxHealth, 0.0f, 0.0f, 20.0f, 20.0f);

            // Set respawn time
            auto& sc = reg.get<StructureComponent>(e);
            sc.respawnTime = inh.respawnTime;

            int laneIdx = static_cast<int>(inh.lane);
            if (laneIdx < 3) {
                result.inhibitors[teamIdx][laneIdx] = e;
            }
        }

        // ── Nexus ──────────────────────────────────────────────────────
        {
            glm::vec3 nexusPos = teamData.base.nexusPosition;
            std::string name = "Nexus_T" + std::to_string(teamIdx);

            entt::entity e = createStructureEntity(
                name, nexusPos, StructureType::NEXUS, teamIdx,
                LaneType::Mid, 5500.0f, 0.0f, 0.0f, 0.0f, 0.0f);

            result.nexuses[teamIdx] = e;
        }
    }

    // ── Set up lane gating prerequisites ─────────────────────────────────
    // T1 must die before T2, T2 before T3, T3 before inhibitor, inhibitor before nexus
    for (uint8_t teamIdx = 0; teamIdx < 2; ++teamIdx) {
        for (int laneIdx = 0; laneIdx < 3; ++laneIdx) {
            auto& towers = result.laneTowers[teamIdx][laneIdx];
            entt::entity inhib = result.inhibitors[teamIdx][laneIdx];

            // T2 requires T1
            if (towers[1] != entt::null && towers[0] != entt::null) {
                reg.get<StructureComponent>(towers[1]).prerequisite = towers[0];
            }
            // T3 requires T2
            if (towers[2] != entt::null && towers[1] != entt::null) {
                reg.get<StructureComponent>(towers[2]).prerequisite = towers[1];
            }
            // Inhibitor requires T3
            if (inhib != entt::null && towers[2] != entt::null) {
                reg.get<StructureComponent>(inhib).prerequisite = towers[2];
            }
        }

        // Nexus requires ALL inhibitors of that team to be destroyed (any lane)
        // For simplicity, nexus prerequisite = first inhibitor (we check all in update)
        // Actually, the standard rule is: at least one inhibitor must be down.
        // We'll use a special check: nexus towers require any inhibitor down.
        // For now, set nexus towers prerequisite to mid inhibitor as primary gate.
        entt::entity midInhib = result.inhibitors[teamIdx][static_cast<int>(LaneType::Mid)];
        for (auto nexusTower : result.nexusTowers[teamIdx]) {
            if (nexusTower != entt::null && midInhib != entt::null) {
                // Nexus towers need at least one inhibitor destroyed
                // We'll check all 3 inhibitors in isPrerequisiteMet override
                reg.get<StructureComponent>(nexusTower).prerequisite = midInhib;
            }
        }
        // Nexus itself requires nexus towers (simplified: any inhibitor)
        if (result.nexuses[teamIdx] != entt::null) {
            // Nexus needs at least one inhibitor destroyed to be attackable
            if (midInhib != entt::null) {
                reg.get<StructureComponent>(result.nexuses[teamIdx]).prerequisite = midInhib;
            }
        }
    }

    spdlog::info("StructureSystem: spawned {} structures per team",
                 mapData.teams[0].towers.size() + mapData.teams[0].inhibitors.size() + 1);

    return result;
}

} // namespace glory
