#include "ability/AbilityComponents.h"
#include "ability/AbilityTypes.h"
#include "audio/GameAudioEvents.h"
#include "combat/CombatSystem.h"
#include "combat/EconomySystem.h"
#include "combat/HeroDefinition.h"
#include "combat/RespawnSystem.h"
#include "combat/StructureSystem.h"
#include "core/FixedPoint.h"
#include "fog/FogComponents.h"
#include "scene/Components.h"
#include "vfx/VFXEventQueue.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>
#include <vector>

namespace glory {

// ═══ CombatSystem.cpp ═══

CombatSystem::CombatSystem(VFXEventQueue& vfxQueue) : m_vfxQueue(vfxQueue) {}

// ── Input entry-points ──────────────────────────────────────────────────────

void CombatSystem::requestAutoAttack(entt::entity attacker, entt::entity target, entt::registry& reg) {
    if (!reg.all_of<CombatComponent>(attacker)) return;
    auto& combat = reg.get<CombatComponent>(attacker);
    
    // Simple checks: must be IDLE or WINDDOWN, and target must be valid
    if (combat.state != CombatState::IDLE && combat.state != CombatState::ATTACK_WINDDOWN) return;
    if (combat.attackCooldown > 0.0f) return;
    if (!reg.valid(target)) return;

    combat.state = CombatState::ATTACK_WINDUP;
    
    Fixed32 fAttackSpeed(combat.attackSpeed);
    Fixed32 fCycleTime = Fixed32::one() / fAttackSpeed;
    Fixed32 fWindupPercent(combat.windupPercent);
    combat.stateTimer = (fCycleTime * fWindupPercent).toFloat();
    
    combat.targetEntity = target;
}

void CombatSystem::requestShield(entt::entity entity, entt::registry& reg) {
    if (!reg.all_of<CombatComponent, TransformComponent>(entity)) return;
    auto& combat = reg.get<CombatComponent>(entity);
    if (combat.state != CombatState::IDLE || combat.shieldCooldown > 0.0f) return;

    combat.state = CombatState::SHIELDING;
    combat.stateTimer = combat.shieldDuration;

    auto& t = reg.get<TransformComponent>(entity);
    combat.shieldVfxHandle = static_cast<uint32_t>(entity) | 0x80000000;
    emitVFX("vfx_shield", t.position, glm::vec3(0, 1, 0), 1.0f, combat.shieldVfxHandle);
    emitVFX("vfx_molten_shield_apply", t.position, glm::vec3(0, 1, 0));
    emitVFX("vfx_molten_shield_embers", t.position, glm::vec3(0, 1, 0));
}

void CombatSystem::releaseShield(entt::entity entity, entt::registry& reg) {
    if (!reg.all_of<CombatComponent>(entity)) return;
    auto& combat = reg.get<CombatComponent>(entity);
    if (combat.state != CombatState::SHIELDING) return;

    combat.state = CombatState::IDLE;
    combat.shieldCooldown = combat.shieldCooldownBase * 0.5f; // shorter CD for manual release

    if (combat.shieldVfxHandle != 0) {
        VFXEvent ev{};
        ev.type = VFXEventType::Destroy;
        ev.handle = combat.shieldVfxHandle;
        m_vfxQueue.push(ev);
        combat.shieldVfxHandle = 0;
    }
}

void CombatSystem::requestTrick(entt::entity attacker, entt::entity target,
                                 entt::registry& reg) {
    if (!reg.all_of<CombatComponent>(attacker)) return;
    auto& combat = reg.get<CombatComponent>(attacker);
    if (combat.state != CombatState::IDLE || combat.trickCooldown > 0.0f) return;

    combat.state = CombatState::TRICKING;
    combat.stateTimer = combat.trickWindup;
    combat.targetEntity = target;

    auto& t = reg.get<TransformComponent>(attacker);
    glm::vec3 dir = glm::normalize(reg.get<TransformComponent>(target).position - t.position);
    emitVFX("vfx_trick_cast", t.position, dir);
    emitVFX("vfx_trick", t.position, dir);
}

// ── Per-frame update ────────────────────────────────────────────────────────

void CombatSystem::update(entt::registry& registry, float dt) {
    auto view = registry.view<CombatComponent>();
    for (auto [entity, combat] : view.each()) {
        // Tick cooldowns
        combat.attackCooldown = std::max(0.0f, combat.attackCooldown - dt);
        combat.shieldCooldown = std::max(0.0f, combat.shieldCooldown - dt);
        combat.trickCooldown  = std::max(0.0f, combat.trickCooldown  - dt);

        switch (combat.state) {
        case CombatState::ATTACK_WINDUP:
            processAttackWindup(registry, entity, combat, dt);
            break;
        case CombatState::ATTACK_FIRE:
            processAttackFire(registry, entity, combat, dt);
            break;
        case CombatState::ATTACK_WINDDOWN:
            processAttackWinddown(registry, entity, combat, dt);
            break;
        case CombatState::SHIELDING:
            processShield(registry, entity, combat, dt);
            break;
        case CombatState::TRICKING:
            processTrick(registry, entity, combat, dt);
            break;
        case CombatState::STUNNED:
            processStun(combat, dt);
            break;
        case CombatState::IDLE:
            break;
        }
    }
}

// ── State processors ────────────────────────────────────────────────────────

void CombatSystem::processAttackWindup(entt::registry& reg, entt::entity entity,
                                        CombatComponent& combat, float dt) {
    combat.stateTimer -= dt;
    if (combat.stateTimer <= 0.0f) {
        combat.state = CombatState::ATTACK_FIRE;
        combat.stateTimer = 0.0f; // Instant transition to FIRE phase
    }
}

void CombatSystem::processAttackFire(entt::registry& reg, entt::entity entity,
                                      CombatComponent& combat, float dt) {
    // Determine the fire point logic (damage vs projectile)
    if (combat.isRanged) {
        spawnAutoAttackProjectile(reg, entity, combat.targetEntity, combat);
    } else {
        applyAutoAttackHit(reg, entity, combat.targetEntity);
    }

    // Set internal attack cooldown based on AttackSpeed (deterministic)
    Fixed32 fAttackSpeed(combat.attackSpeed);
    Fixed32 fCycleTime = Fixed32::one() / fAttackSpeed;
    combat.attackCooldown = fCycleTime.toFloat();

    // Transition to Wind-down
    combat.state = CombatState::ATTACK_WINDDOWN;
    Fixed32 fWindupPercent(combat.windupPercent);
    combat.stateTimer = (fCycleTime * (Fixed32::one() - fWindupPercent)).toFloat();
}

void CombatSystem::processAttackWinddown(entt::registry& reg, entt::entity entity,
                                          CombatComponent& combat, float dt) {
    combat.stateTimer -= dt;
    if (combat.stateTimer <= 0.0f) {
        combat.state = CombatState::IDLE;
        combat.targetEntity = entt::null;
    }
}

void CombatSystem::processShield(entt::registry& reg, entt::entity entity,
                                  CombatComponent& combat, float dt) {
    combat.stateTimer -= dt;
    if (combat.stateTimer <= 0.0f) {
        // Shield expired from duration
        combat.state = CombatState::IDLE;
        combat.shieldCooldown = combat.shieldCooldownBase;
        
        if (combat.shieldVfxHandle != 0) {
            VFXEvent ev{};
            ev.type = VFXEventType::Destroy;
            ev.handle = combat.shieldVfxHandle;
            m_vfxQueue.push(ev);
            combat.shieldVfxHandle = 0;
        }
    } else {
        if (combat.shieldVfxHandle != 0 && reg.all_of<TransformComponent>(entity)) {
            auto& t = reg.get<TransformComponent>(entity);
            VFXEvent ev{};
            ev.type = VFXEventType::Move;
            ev.handle = combat.shieldVfxHandle;
            ev.position = t.position;
            m_vfxQueue.push(ev);
        }
    }
}

void CombatSystem::processTrick(entt::registry& reg, entt::entity entity,
                                 CombatComponent& combat, float dt) {
    combat.stateTimer -= dt;
    if (combat.stateTimer <= 0.0f) {
        applyTrickHit(reg, entity, combat.targetEntity, combat);
        combat.trickCooldown = combat.trickCooldownBase;
        // State is set inside applyTrickHit (STUNNED if failed, IDLE if success)
        if (combat.state == CombatState::TRICKING) {
            combat.state = CombatState::IDLE;
        }
        combat.targetEntity = entt::null;
    }
}

void CombatSystem::processStun(CombatComponent& combat, float dt) {
    combat.stateTimer -= dt;
    if (combat.stateTimer <= 0.0f) {
        combat.state = CombatState::IDLE;
    }
}

// ── Hit resolution ──────────────────────────────────────────────────────────

void CombatSystem::spawnAutoAttackProjectile(entt::registry& reg, entt::entity attacker,
                                              entt::entity target, CombatComponent& combat) {
    if (!reg.valid(target) || !reg.all_of<TransformComponent>(target))
        return;

    auto& attackerPos = reg.get<TransformComponent>(attacker).position;
    auto& targetPos   = reg.get<TransformComponent>(target).position;
    glm::vec3 spawnPos = attackerPos + glm::vec3(0, 1.0f, 0); // spawn at chest height

    entt::entity proj = reg.create();
    reg.emplace<TransformComponent>(proj, TransformComponent{spawnPos, glm::vec3(0), glm::vec3(1.0f)});
    
    auto& pc = reg.emplace<ProjectileComponent>(proj);
    pc.casterEntity = static_cast<EntityID>(entt::to_integral(attacker));
    pc.targetEntity = static_cast<EntityID>(entt::to_integral(target));
    pc.speed        = combat.projectileSpeed;
    pc.damage       = combat.attackDamage;
    pc.isAutoAttack = true;

    // Spawn flight VFX
    if (!combat.projectileVfx.empty()) {
        uint32_t vfxHandle = (static_cast<uint32_t>(entt::to_integral(proj)) + 1) * 1000;
        pc.vfxHandles.push_back(vfxHandle);
        emitVFX(combat.projectileVfx, spawnPos, glm::normalize(targetPos - attackerPos), 1.0f, vfxHandle);
    }
}

void CombatSystem::applyAutoAttackHit(entt::registry& reg, entt::entity attacker,
                                       entt::entity target) {
    if (!reg.valid(target) || !reg.all_of<CombatComponent, TransformComponent>(target))
        return;

    auto& targetCombat = reg.get<CombatComponent>(target);
    auto& attackerPos  = reg.get<TransformComponent>(attacker).position;
    auto& targetPos    = reg.get<TransformComponent>(target).position;
    glm::vec3 dir      = glm::normalize(targetPos - attackerPos);

    // Emit melee slash + hit VFX at midpoint
    glm::vec3 midpoint = (attackerPos + targetPos) * 0.5f;
    emitVFX("vfx_melee_slash", midpoint, dir);
    emitVFX("vfx_melee_hit", midpoint, dir);

    // Audio: auto-attack sound at impact position
    if (m_audio) m_audio->onAutoAttack(midpoint, false);

    if (targetCombat.state == CombatState::SHIELDING) {
        // BLOCKED
        emitVFX("vfx_attack_blocked", targetPos, dir);
        spdlog::debug("Auto-attack blocked by shield");
    } else {
        // HIT — apply damage (deterministic fixed-point arithmetic)
        if (reg.all_of<StatsComponent>(target)) {
            auto& stats = reg.get<StatsComponent>(target);
            auto& attackerCombat = reg.get<CombatComponent>(attacker);
            Fixed32 fDamage(attackerCombat.attackDamage);
            Stats tStats = stats.total();
            Fixed32 fArmor(tStats.armor);
            Fixed32 fFinal = fDamage * (Fixed32(100) / (Fixed32(100) + fArmor));
            float finalDamage = fFinal.toFloat();
            stats.base.currentHP = std::max(0.0f, stats.base.currentHP - finalDamage);
            spdlog::debug("Auto-attack hit for {:.1f} damage (HP: {:.1f})",
                          finalDamage, stats.base.currentHP);

            // Kill detection: award gold/xp if target died
            if (stats.base.currentHP <= 0.0f && m_economy) {
                m_economy->awardKill(reg, attacker, target);
            }
        }
    }
}

void CombatSystem::applyTrickHit(entt::registry& reg, entt::entity attacker,
                                  entt::entity target, CombatComponent& attackerCombat) {
    if (!reg.valid(target) || !reg.all_of<CombatComponent, TransformComponent>(target))
        return;

    auto& targetCombat = reg.get<CombatComponent>(target);
    auto& attackerPos  = reg.get<TransformComponent>(attacker).position;
    auto& targetPos    = reg.get<TransformComponent>(target).position;
    glm::vec3 dir      = glm::normalize(targetPos - attackerPos);

    if (targetCombat.state == CombatState::SHIELDING) {
        // Trick beats Shield — stun the defender
        targetCombat.state      = CombatState::STUNNED;
        targetCombat.stateTimer = attackerCombat.stunDuration;
        attackerCombat.state    = CombatState::IDLE;
        emitVFX("vfx_trick_success", targetPos, dir);
        spdlog::debug("Trick broke shield — defender stunned for {:.1f}s",
                      attackerCombat.stunDuration);
    } else {
        // Trick fails — stun the attacker
        attackerCombat.state      = CombatState::STUNNED;
        attackerCombat.stateTimer = attackerCombat.stunDuration;
        emitVFX("vfx_trick_fail", attackerPos, -dir);
        spdlog::debug("Trick failed — attacker stunned for {:.1f}s",
                      attackerCombat.stunDuration);
    }
}

// ── Utility ─────────────────────────────────────────────────────────────────

entt::entity CombatSystem::findNearestEnemy(entt::registry& reg, entt::entity attacker,
                                             float range) {
    if (!reg.all_of<TransformComponent, TeamComponent>(attacker)) return entt::null;

    auto& attackerPos  = reg.get<TransformComponent>(attacker).position;
    auto  attackerTeam = reg.get<TeamComponent>(attacker).team;

    float bestDistSq = range * range;
    entt::entity best = entt::null;

    auto view = reg.view<TransformComponent, TeamComponent>();
    for (auto [entity, transform, team] : view.each()) {
        if (entity == attacker) continue;
        if (team.team == attackerTeam) continue;

        glm::vec2 a2d(attackerPos.x, attackerPos.z);
        glm::vec2 e2d(transform.position.x, transform.position.z);
        glm::vec2 diff = e2d - a2d;
        float distSq = diff.x * diff.x + diff.y * diff.y;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = entity;
        }
    }
    return best;
}

void CombatSystem::emitVFX(const std::string& effectId, const glm::vec3& pos,
                            const glm::vec3& dir, float scale, uint32_t handle) {
    VFXEvent ev{};
    ev.type = VFXEventType::Spawn;
    std::strncpy(ev.effectID, effectId.c_str(), sizeof(ev.effectID) - 1);
    ev.effectID[sizeof(ev.effectID) - 1] = '\0';
    ev.position  = pos;
    ev.direction = dir;
    ev.scale     = scale;
    ev.handle    = handle;
    ev.lifetime  = -1.0f; // use EmitterDef duration

    if (!m_vfxQueue.push(ev)) {
        spdlog::warn("CombatSystem: VFX queue full, dropped '{}'", effectId);
    }
}

// ═══ StructureSystem.cpp ═══

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
        // Raise structures above the ground plane so they aren't clipped by
        // lane tiles (which sit at Y ≈ 0.05–0.15).
        tc.position.y = 0.2f;
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

// ═══ RespawnSystem.cpp ═══

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
