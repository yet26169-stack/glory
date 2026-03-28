#include "ability/AbilityComponents.h"
#include "ability/AbilitySystem.h"
#include "ability/AbilityTypes.h"
#include "combat/CombatComponents.h"
#include "combat/CombatSystem.h"
#include "core/Profiler.h"
#include "combat/EconomySystem.h"
#include "combat/MinionWaveSystem.h"
#include "combat/NPCBehaviorSystem.h"
#include "combat/RespawnSystem.h"
#include "fog/FogComponents.h"
#include "nav/PathfindingSystem.h"
#include "physics/RigidBodyComponent.h"
#include "scene/Components.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

namespace glory {

// ═══ MinionWaveSystem.cpp ═══

// ═════════════════════════════════════════════════════════════════════════════
// Init — store map data
// ═════════════════════════════════════════════════════════════════════════════
void MinionWaveSystem::init(const MapData& mapData) {
    m_mapData = mapData;
    m_nextWaveTime = FIRST_WAVE_TIME;
    m_waveNumber = 0;
    spdlog::info("[MinionWave] Initialized — first wave at {:.0f}s", FIRST_WAVE_TIME);
}

// ═════════════════════════════════════════════════════════════════════════════
// Stat scaling per wave
// ═════════════════════════════════════════════════════════════════════════════
MinionWaveSystem::MinionStats MinionWaveSystem::scaledStats(MinionType type,
                                                             uint32_t wave) const {
    MinionStats s{};
    switch (type) {
    case MinionType::MELEE:
        s.hp          = 480.0f + 27.0f * wave;
        s.ad          = 12.0f + 0.5f * wave;
        s.moveSpeed   = 3.0f;
        s.attackRange = 2.0f;
        s.isRanged    = false;
        s.type        = MinionType::MELEE;
        s.goldReward  = 20;
        s.xpReward    = 60;
        break;
    case MinionType::RANGED:
        s.hp          = 290.0f + 16.0f * wave;
        s.ad          = 23.0f + 0.5f * wave;
        s.moveSpeed   = 3.0f;
        s.attackRange = 5.0f;
        s.isRanged    = true;
        s.type        = MinionType::RANGED;
        s.goldReward  = 15;
        s.xpReward    = 30;
        break;
    case MinionType::CANNON:
        s.hp          = 700.0f + 27.0f * wave;
        s.ad          = 40.0f + 1.0f * wave;
        s.moveSpeed   = 3.0f;
        s.attackRange = 4.0f;
        s.isRanged    = true;
        s.type        = MinionType::CANNON;
        s.goldReward  = 45;
        s.xpReward    = 90;
        break;
    }
    return s;
}

// ═════════════════════════════════════════════════════════════════════════════
// Update — spawn waves + move/aggro existing minions
// ═════════════════════════════════════════════════════════════════════════════
void MinionWaveSystem::update(entt::registry& reg, float dt, float gameTime) {
    // Spawn wave if it's time
    if (gameTime >= m_nextWaveTime && m_spawnCfg.ready) {
        spawnWave(reg);
        m_nextWaveTime += WAVE_INTERVAL;
    }

    // Update existing wave minions (movement + aggro)
    updateMovementAndAggro(reg, dt);
}

// ═════════════════════════════════════════════════════════════════════════════
// Spawn one full wave: 3 melee + 3 ranged + (optional cannon) × 3 lanes × 2 teams
// ═════════════════════════════════════════════════════════════════════════════
void MinionWaveSystem::spawnWave(entt::registry& reg) {
    ++m_waveNumber;
    bool hasCannon = (m_waveNumber % 3 == 0);

    spdlog::info("[MinionWave] Wave {} spawning (cannon={})", m_waveNumber, hasCannon);

    for (uint8_t team = 0; team < 2; ++team) {
        glm::vec4 tint = (team == 0) ? BLUE_TINT : RED_TINT;
        glm::vec3 nexusPos = m_mapData.teams[team].base.nexusPosition;
        // Slight offset so minions don't stack exactly on the nexus
        glm::vec3 spawnBase = nexusPos;

        for (uint8_t lane = 0; lane < 3; ++lane) {
            // Get lane start direction for a slight spread
            const auto& waypoints = m_mapData.teams[team].lanes[lane].waypoints;
            glm::vec3 laneDir{0.0f, 0.0f, 1.0f};
            if (waypoints.size() >= 2)
                laneDir = glm::normalize(waypoints[1] - waypoints[0]);

            // Use first waypoint as spawn position (closer to lane start)
            glm::vec3 laneSpawn = waypoints.empty() ? spawnBase : waypoints[0];

            int idx = 0;
            auto spawnAt = [&](MinionType type) {
                MinionStats stats = scaledStats(type, m_waveNumber);
                // Spread minions slightly perpendicular to lane direction
                glm::vec3 perp(-laneDir.z, 0.0f, laneDir.x);
                float offset = (static_cast<float>(idx) - 3.0f) * 0.8f;
                glm::vec3 pos = laneSpawn + perp * offset +
                                laneDir * (static_cast<float>(idx) * 0.5f);
                pos.y = 0.0f; // ground level
                spawnMinion(reg, team, lane, pos, stats, tint);
                ++idx;
            };

            // 3 melee
            for (int i = 0; i < 3; ++i) spawnAt(MinionType::MELEE);
            // 3 ranged
            for (int i = 0; i < 3; ++i) spawnAt(MinionType::RANGED);
            // 1 cannon every 3rd wave
            if (hasCannon) spawnAt(MinionType::CANNON);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Spawn a single minion entity with all required components
// ═════════════════════════════════════════════════════════════════════════════
entt::entity MinionWaveSystem::spawnMinion(entt::registry& reg,
                                            uint8_t team, uint8_t lane,
                                            glm::vec3 pos,
                                            const MinionStats& stats,
                                            const glm::vec4& tint) {
    static uint32_t minionId = 0;

    entt::entity e = reg.create();

    // Tag
    std::string name = (team == 0 ? "Blue" : "Red");
    name += "_";
    switch (stats.type) {
    case MinionType::MELEE:  name += "Melee";  break;
    case MinionType::RANGED: name += "Ranged"; break;
    case MinionType::CANNON: name += "Cannon"; break;
    }
    name += "_" + std::to_string(minionId++);
    reg.emplace_or_replace<TagComponent>(e, TagComponent{name});

    // Transform
    auto& tc = reg.emplace_or_replace<TransformComponent>(e);
    tc.position = pos;
    tc.scale    = glm::vec3(0.025f);
    tc.rotation = glm::vec3(0.0f);

    // Team
    Team teamEnum = (team == 0) ? Team::PLAYER : Team::ENEMY;
    reg.emplace_or_replace<TeamComponent>(e, TeamComponent{teamEnum});

    // Tint for team-coloured rendering
    reg.emplace_or_replace<TintComponent>(e, TintComponent{tint});

    // Stats
    StatsComponent sc;
    sc.base.maxHP        = stats.hp;
    sc.base.currentHP    = stats.hp;
    sc.base.armor        = 10.0f;
    sc.base.magicResist  = 10.0f;
    sc.base.attackDamage = stats.ad;
    reg.emplace_or_replace<StatsComponent>(e, sc);

    // Combat
    CombatComponent cc;
    cc.attackRange  = stats.attackRange;
    cc.attackSpeed  = 0.8f;
    cc.attackDamage = stats.ad;
    cc.isRanged     = stats.isRanged;
    if (stats.isRanged) cc.projectileSpeed = 15.0f;
    reg.emplace_or_replace<CombatComponent>(e, cc);

    // Minion economy (kill rewards)
    reg.emplace_or_replace<MinionComponent>(e, MinionComponent{stats.type, stats.goldReward, stats.xpReward});
    reg.emplace_or_replace<EconomyComponent>(e, EconomyComponent{0, 0, 1});

    // Movement
    reg.emplace_or_replace<UnitComponent>(e, UnitComponent{UnitComponent::State::MOVING, pos, stats.moveSpeed});
    reg.emplace_or_replace<CharacterComponent>(e, CharacterComponent{pos, stats.moveSpeed});
    reg.emplace_or_replace<SelectableComponent>(e, SelectableComponent{false, 1.0f});

    // Wave AI component
    WaveMinionComponent wmc;
    wmc.teamIndex = team;
    wmc.laneIndex = lane;
    wmc.type      = stats.type;
    reg.emplace_or_replace<WaveMinionComponent>(e, wmc);

    // Minions die permanently (no respawn, entity destroyed)
    reg.emplace_or_replace<RespawnComponent>(e, RespawnComponent{
        LifeState::ALIVE, 0.f, 0.f, glm::vec3(0.f), false /*isHero=false*/
    });

    // FoW vision: minions have small sight radius
    reg.emplace_or_replace<VisionComponent>(e, VisionComponent{6.0f});

    // Physics body for collision separation (prevents minion stacking)
    RigidBodyComponent rbc;
    rbc.collisionRadius = 0.4f;
    rbc.mass            = 1.0f;
    rbc.linearDamping   = 0.9f;  // high damping — physics only separates, doesn't propel
    rbc.restitution     = 0.0f;
    reg.emplace_or_replace<RigidBodyComponent>(e, rbc);

    // Mesh + material + animation — pick config based on minion type
    const WaveSpawnConfig& cfg = (stats.type != MinionType::MELEE && m_casterSpawnCfg.ready)
                                 ? m_casterSpawnCfg : m_spawnCfg;
    reg.emplace_or_replace<GPUSkinnedMeshComponent>(e, GPUSkinnedMeshComponent{cfg.meshIndex});
    reg.emplace_or_replace<MaterialComponent>(e, MaterialComponent{
        cfg.texIndex, cfg.flatNormIndex, 0.0f, 0.0f, 0.5f, 0.2f});

    // GPU skinning only needs the joint hierarchy (for matrix computation) — not raw vertex data.
    // Copying skinVertices/bindPoseVertices per entity caused ~53 MB/wave allocation overhead.
    SkeletonComponent skelComp;
    skelComp.skeleton = cfg.skeleton;

    AnimationComponent animComp;
    animComp.player.setSkeleton(&skelComp.skeleton);
    animComp.clips = cfg.clips;
    if (!animComp.clips.empty()) {
        animComp.activeClipIndex = 0;
        animComp.player.setClip(&animComp.clips[0]);
    }

    reg.emplace_or_replace<SkeletonComponent>(e, std::move(skelComp));
    reg.emplace_or_replace<AnimationComponent>(e, std::move(animComp));

    // Re-point raw pointers to registry-owned copies
    auto& regSkel = reg.get<SkeletonComponent>(e);
    auto& regAnim = reg.get<AnimationComponent>(e);
    regAnim.player.setSkeleton(&regSkel.skeleton);
    if (!regAnim.clips.empty())
        regAnim.player.setClip(&regAnim.clips[regAnim.activeClipIndex]);

    return e;
}

// ═════════════════════════════════════════════════════════════════════════════
// Movement + aggro AI each tick
// ═════════════════════════════════════════════════════════════════════════════
void MinionWaveSystem::updateMovementAndAggro(entt::registry& reg, float dt) {
    GLORY_ZONE_N("MinionAggro");
    auto view = reg.view<WaveMinionComponent, TransformComponent,
                         CharacterComponent, StatsComponent, CombatComponent>();

    // Separation: pre-collect minion positions to avoid nested view iteration
    constexpr float SEPARATION_RADIUS  = 1.0f;
    constexpr float SEPARATION_RADIUS2 = SEPARATION_RADIUS * SEPARATION_RADIUS;
    constexpr float SEPARATION_WEIGHT  = 3.0f;

    struct MinionPos { entt::entity e; uint8_t team; glm::vec3 pos; };
    std::vector<MinionPos> minionPositions;
    for (auto&& [e, wmc, tc] : reg.view<WaveMinionComponent, TransformComponent>().each()) {
        minionPositions.push_back({e, wmc.teamIndex, tc.position});
    }

    auto computeSeparation = [&](entt::entity self, glm::vec3 selfPos,
                                 uint8_t selfTeam) -> glm::vec3 {
        glm::vec3 sep{0.0f};
        int count = 0;
        for (const auto& mp : minionPositions) {
            if (mp.e == self || mp.team != selfTeam) continue;
            glm::vec3 diff = selfPos - mp.pos;
            diff.y = 0.0f;
            float d2 = glm::dot(diff, diff);
            if (d2 > 0.001f && d2 < SEPARATION_RADIUS2) {
                sep += diff / std::sqrt(d2);
                ++count;
            }
        }
        if (count > 0) sep /= static_cast<float>(count);
        return sep * SEPARATION_WEIGHT;
    };

    for (auto&& [entity, wmc, transform, character, stats, combat]
         : view.each()) {

        if (stats.base.currentHP <= 0.0f) continue;

        // Clamp to ground plane — physics applies gravity but minions are ground units.
        // Zero Y velocity so gravity never accumulates between frames.
        if (auto* rb = reg.try_get<RigidBodyComponent>(entity)) {
            rb->linearVelocity.y = 0.0f;
            rb->accumulatedForce.y = 0.0f;
        }
        transform.position.y = 0.2f;

        glm::vec3 pos = transform.position;

        // ── Hero aggro timer countdown ──────────────────────────────────
        if (wmc.heroAggroTimer > 0.0f) {
            wmc.heroAggroTimer -= dt;
            if (wmc.heroAggroTimer <= 0.0f) {
                wmc.heroAggroTarget = entt::null;
                wmc.heroAggroTimer = 0.0f;
            }
        }

        // ── Validate hero aggro target ──────────────────────────────────
        if (wmc.heroAggroTarget != entt::null) {
            if (!reg.valid(wmc.heroAggroTarget)) {
                wmc.heroAggroTarget = entt::null;
                wmc.heroAggroTimer = 0.0f;
            } else {
                auto* tgt = reg.try_get<TransformComponent>(wmc.heroAggroTarget);
                auto* tgtStats = reg.try_get<StatsComponent>(wmc.heroAggroTarget);
                if (!tgt || !tgtStats || tgtStats->base.currentHP <= 0.0f) {
                    wmc.heroAggroTarget = entt::null;
                    wmc.heroAggroTimer = 0.0f;
                } else {
                    float dist = glm::length(tgt->position - pos);
                    if (dist > wmc.heroAggroLeash) {
                        wmc.heroAggroTarget = entt::null;
                        wmc.heroAggroTimer = 0.0f;
                    }
                }
            }
        }

        // ── Helper: validate a target entity ────────────────────────────
        auto isTargetValid = [&](entt::entity t, float maxRange) -> bool {
            if (t == entt::null || !reg.valid(t)) return false;
            auto* tgt = reg.try_get<TransformComponent>(t);
            auto* tgtStats = reg.try_get<StatsComponent>(t);
            auto* tgtTeam = reg.try_get<TeamComponent>(t);
            if (!tgt || !tgtStats || !tgtTeam) return false;
            if (tgtStats->base.currentHP <= 0.0f) return false;
            Team myTeam = (wmc.teamIndex == 0) ? Team::PLAYER : Team::ENEMY;
            if (tgtTeam->team == myTeam || tgtTeam->team == Team::NEUTRAL) return false;
            float distSq = glm::dot(
                glm::vec2(tgt->position.x - pos.x, tgt->position.z - pos.z),
                glm::vec2(tgt->position.x - pos.x, tgt->position.z - pos.z));
            return distSq <= maxRange * maxRange;
        };

        // ── Resolve effective target (hero aggro overrides) ─────────────
        entt::entity resolvedTarget = entt::null;
        if (wmc.heroAggroTarget != entt::null && reg.valid(wmc.heroAggroTarget)) {
            resolvedTarget = wmc.heroAggroTarget;
        } else if (isTargetValid(wmc.aggroTarget, wmc.acquisitionRange * 1.5f)) {
            resolvedTarget = wmc.aggroTarget;
        } else {
            wmc.retargetCooldown -= dt;
            if (wmc.retargetCooldown <= 0.0f) {
                resolvedTarget = findTarget(reg, entity, wmc, pos);
                wmc.retargetCooldown = WaveMinionComponent::RETARGET_INTERVAL;
            }
        }
        wmc.aggroTarget = resolvedTarget;
        combat.targetEntity = resolvedTarget;

        // ── FSM state transitions ───────────────────────────────────────
        switch (wmc.aiState) {

        case MinionAIState::LANE_MARCH: {
            if (resolvedTarget != entt::null) {
                wmc.laneLeavePos = pos;
                wmc.aiState = MinionAIState::CHASE_TARGET;
                break;
            }
            if (m_pathfinding && m_pathfinding->flowFieldsReady()) {
                glm::vec2 dir2 = m_pathfinding->sampleFlowField(
                    wmc.teamIndex, wmc.laneIndex,
                    glm::vec2(pos.x, pos.z));
                if (glm::length(dir2) > 0.01f) {
                    glm::vec3 dir3(dir2.x, 0.0f, dir2.y);
                    glm::vec3 sep = computeSeparation(entity, pos, wmc.teamIndex);
                    glm::vec3 move = dir3 + sep;
                    move.y = 0.0f;
                    float len = glm::length(move);
                    if (len > 0.01f) move /= len;
                    transform.position += move * character.moveSpeed * dt;
                    transform.position.y = 0.2f;
                    character.currentYaw = std::atan2(move.x, move.z);
                    character.hasTarget = true;
                    character.targetPosition = pos + dir3 * 5.0f;
                }
            }
            break;
        }

        case MinionAIState::CHASE_TARGET: {
            if (resolvedTarget == entt::null) {
                wmc.aiState = MinionAIState::RETURN_TO_LANE;
                break;
            }
            // Leash: hero aggro ignores distance leash
            float chaseDist = glm::length(pos - wmc.laneLeavePos);
            if (chaseDist > WaveMinionComponent::MAX_CHASE_DIST
                && wmc.heroAggroTarget == entt::null) {
                wmc.aggroTarget = entt::null;
                combat.targetEntity = entt::null;
                wmc.aiState = MinionAIState::RETURN_TO_LANE;
                break;
            }

            if (!reg.valid(resolvedTarget) || !reg.all_of<TransformComponent>(resolvedTarget)) {
                wmc.aggroTarget = entt::null;
                combat.targetEntity = entt::null;
                wmc.aiState = MinionAIState::RETURN_TO_LANE;
                break;
            }
            {
            auto& targetTransform = reg.get<TransformComponent>(resolvedTarget);
            glm::vec3 toTarget = targetTransform.position - pos;
            float dist = glm::length(toTarget);

            if (dist <= combat.attackRange) {
                wmc.aiState = MinionAIState::ATTACKING;
                glm::vec3 dir = toTarget / std::max(dist, 0.001f);
                character.currentYaw = std::atan2(dir.x, dir.z);
                character.hasTarget = true;
                character.targetPosition = targetTransform.position;
            } else {
                glm::vec3 dir = toTarget / dist;
                glm::vec3 sep = computeSeparation(entity, pos, wmc.teamIndex);
                glm::vec3 move = dir + sep;
                move.y = 0.0f;
                float len = glm::length(move);
                if (len > 0.01f) move /= len;
                transform.position += move * character.moveSpeed * dt;
                transform.position.y = 0.2f;
                character.currentYaw = std::atan2(move.x, move.z);
                character.hasTarget = true;
                character.targetPosition = targetTransform.position;
            }
            }
            break;
        }

        case MinionAIState::ATTACKING: {
            if (resolvedTarget == entt::null) {
                // Immediate retarget attempt (bypass cooldown)
                wmc.retargetCooldown = 0.0f;
                entt::entity newTarget = findTarget(reg, entity, wmc, pos);
                if (newTarget != entt::null) {
                    wmc.aggroTarget = newTarget;
                    combat.targetEntity = newTarget;
                    wmc.aiState = MinionAIState::CHASE_TARGET;
                } else {
                    wmc.aiState = MinionAIState::RETURN_TO_LANE;
                }
                break;
            }

            if (!reg.valid(resolvedTarget) || !reg.all_of<TransformComponent>(resolvedTarget)) {
                wmc.aggroTarget = entt::null;
                combat.targetEntity = entt::null;
                wmc.retargetCooldown = 0.0f;
                wmc.aiState = MinionAIState::RETURN_TO_LANE;
                break;
            }
            auto& targetTransform = reg.get<TransformComponent>(resolvedTarget);
            float dist = glm::length(targetTransform.position - pos);

            if (dist > combat.attackRange * 1.2f) {
                wmc.aiState = MinionAIState::CHASE_TARGET;
            } else {
                glm::vec3 toTarget = targetTransform.position - pos;
                float len = glm::length(toTarget);
                if (len > 0.01f) {
                    glm::vec3 dir = toTarget / len;
                    character.currentYaw = std::atan2(dir.x, dir.z);
                }
                character.hasTarget = true;
                character.targetPosition = targetTransform.position;

                // Drive the CombatSystem auto-attack state machine
                if (m_combat)
                    m_combat->requestAutoAttack(entity, resolvedTarget, reg);
            }
            break;
        }

        case MinionAIState::RETURN_TO_LANE: {
            if (resolvedTarget != entt::null) {
                wmc.laneLeavePos = pos;
                wmc.aiState = MinionAIState::CHASE_TARGET;
                break;
            }
            if (m_pathfinding && m_pathfinding->flowFieldsReady()) {
                glm::vec2 dir2 = m_pathfinding->sampleFlowField(
                    wmc.teamIndex, wmc.laneIndex,
                    glm::vec2(pos.x, pos.z));
                if (glm::length(dir2) > 0.01f) {
                    glm::vec3 dir3(dir2.x, 0.0f, dir2.y);
                    transform.position += dir3 * character.moveSpeed * dt;
                    transform.position.y = 0.2f;
                    character.currentYaw = std::atan2(dir3.x, dir3.z);
                    character.hasTarget = true;
                    character.targetPosition = pos + dir3 * 5.0f;
                    wmc.aiState = MinionAIState::LANE_MARCH;
                } else {
                    wmc.aiState = MinionAIState::LANE_MARCH;
                }
            } else {
                wmc.aiState = MinionAIState::LANE_MARCH;
            }
            break;
        }

        } // switch
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Find best target: enemy minions/structures within aggro range
// ═════════════════════════════════════════════════════════════════════════════
entt::entity MinionWaveSystem::findTarget(entt::registry& reg,
                                           entt::entity self,
                                           const WaveMinionComponent& wmc,
                                           glm::vec3 selfPos) const {
    GLORY_ZONE_N("MinionFindTarget");
    Team myTeam = (wmc.teamIndex == 0) ? Team::PLAYER : Team::ENEMY;
    float scanRadiusSq = wmc.acquisitionRange * wmc.acquisitionRange;

    // Priority buckets: [0]=P1 (highest) .. [6]=P7 (lowest)
    // Each stores best entity + best distSq for that priority
    struct Candidate { entt::entity e = entt::null; float distSq = 1e30f; };
    std::array<Candidate, 7> buckets{};

    auto targets = reg.view<TransformComponent, TeamComponent, StatsComponent>();
    for (auto&& [e, tt, teamComp, tStats] : targets.each()) {
        if (e == self) continue;
        if (teamComp.team == myTeam || teamComp.team == Team::NEUTRAL) continue;
        if (tStats.base.currentHP <= 0.0f) continue;

        glm::vec3 diff = tt.position - selfPos;
        float distSq = diff.x * diff.x + diff.z * diff.z;
        if (distSq > scanRadiusSq) continue;

        bool isMinion = reg.all_of<MinionComponent>(e);
        bool isStructure = reg.all_of<MapComponent>(e);
        bool isChampion = !isMinion && !isStructure;

        int priority = -1;
        auto* enemyCombat = reg.try_get<CombatComponent>(e);
        if (enemyCombat && enemyCombat->targetEntity != entt::null
            && enemyCombat->state != CombatState::IDLE
            && reg.valid(enemyCombat->targetEntity)) {

            auto* victimTeam = reg.try_get<TeamComponent>(enemyCombat->targetEntity);
            if (victimTeam && victimTeam->team == myTeam) {
                bool victimIsMinion = reg.all_of<MinionComponent>(enemyCombat->targetEntity);
                bool victimIsChampion = !victimIsMinion && !reg.all_of<MapComponent>(enemyCombat->targetEntity);

                if (isChampion && victimIsChampion)        priority = 0; // P1
                else if (isMinion && victimIsChampion)     priority = 1; // P2
                else if (isMinion && victimIsMinion)       priority = 2; // P3
                else if (isStructure && victimIsMinion)    priority = 3; // P4
                else if (isChampion && victimIsMinion)     priority = 4; // P5
            }
        }

        if (priority < 0) {
            if (isMinion)         priority = 5; // P6: closest enemy minion
            else if (isChampion)  priority = 6; // P7: closest enemy champion
            else continue; // structures with no aggro context
        }

        if (distSq < buckets[priority].distSq) {
            buckets[priority] = {e, distSq};
        }
    }

    for (auto& b : buckets) {
        if (b.e != entt::null) return b.e;
    }
    return entt::null;
}

// ═════════════════════════════════════════════════════════════════════════════
// Hero-aggro notification: hero attacked allied hero near minions
// ═════════════════════════════════════════════════════════════════════════════
void MinionWaveSystem::notifyHeroAttackedHero(entt::registry& reg,
                                               entt::entity heroAttacker,
                                               entt::entity heroVictim) {
    if (!reg.valid(heroVictim) || !reg.valid(heroAttacker)) return;

    auto* victimTeam = reg.try_get<TeamComponent>(heroVictim);
    auto* victimTransform = reg.try_get<TransformComponent>(heroVictim);
    if (!victimTeam || !victimTransform) return;

    glm::vec3 victimPos = victimTransform->position;

    // Find friendly minions near the victim and redirect them to the attacker
    auto view = reg.view<WaveMinionComponent, TransformComponent, TeamComponent>();
    for (auto&& [e, wmc, tc, teamComp] : view.each()) {
        // Only minions allied to the victim switch aggro
        if (teamComp.team != victimTeam->team) continue;

        float dist = glm::length(tc.position - victimPos);
        if (dist <= wmc.heroAggroLeash) {
            wmc.heroAggroTarget = heroAttacker;
            wmc.heroAggroTimer = WaveMinionComponent::HERO_AGGRO_DURATION;
        }
    }
}

// ═══ NPCBehaviorSystem.cpp ═══

// ── lazyInitAbilities ─────────────────────────────────────────────────────────
void NPCBehaviorSystem::lazyInitAbilities(entt::registry& reg, entt::entity e,
                                          AbilitySystem& abilities,
                                          uint8_t /*abilitySetId*/) {
    // Give the entity a Q ability (fireball works as a generic projectile).
    // All four slots are passed; W/E/R are empty strings → skipped by initEntity.
    abilities.initEntity(reg, e, {"fire_mage_fireball", "", "", ""});
    abilities.setAbilityLevel(reg, e, AbilitySlot::Q, 1);

    // Stagger initial cooldown so the whole wave doesn't fire simultaneously.
    // Each successive minion gets an extra 0.3 s offset (wraps at 3 s).
    if (reg.all_of<AbilityBookComponent>(e)) {
        auto& inst = reg.get<AbilityBookComponent>(e)
                        .abilities[static_cast<size_t>(AbilitySlot::Q)];
        inst.cooldownRemaining = std::fmod(m_initCounter * 0.3f, 3.0f);
    }
    ++m_initCounter;
}

// ── update ────────────────────────────────────────────────────────────────────
void NPCBehaviorSystem::update(entt::registry& reg, float dt,
                               AbilitySystem& abilities) {
    auto view = reg.view<WaveMinionComponent, TransformComponent>();

    for (auto [entity, minion, transform] : view.each()) {
        // Lazy ability init — runs once per entity on first encounter
        if (!reg.all_of<AbilityBookComponent>(entity)) {
            lazyInitAbilities(reg, entity, abilities, minion.abilitySetId);
            // Skip the decision this tick; the ability is on its stagger cooldown
            continue;
        }

        // Tick per-entity decision cooldown
        if (minion.decisionCooldown > 0.0f) {
            minion.decisionCooldown -= dt;
            continue;
        }
        minion.decisionCooldown = DECISION_INTERVAL;

        // Resolve aggro target (prefer hero-aggro override when active)
        entt::entity target = (minion.heroAggroTimer > 0.0f && reg.valid(minion.heroAggroTarget))
                              ? minion.heroAggroTarget
                              : minion.aggroTarget;

        if (!reg.valid(target)) continue;

        // Q ability readiness check
        const auto& book = reg.get<AbilityBookComponent>(entity);
        const auto& qInst = book.abilities[static_cast<size_t>(AbilitySlot::Q)];
        if (!qInst.isReady()) continue;
        if (!qInst.def)       continue;

        // Compute direction to target
        const auto* targetTc = reg.try_get<TransformComponent>(target);
        if (!targetTc) continue;

        glm::vec3 delta = targetTc->position - transform.position;
        float dist = glm::length(delta);
        if (dist < 0.001f) continue;

        // Only fire if target is within the ability's declared cast range
        if (dist > qInst.def->castRange) continue;

        glm::vec3 dir = delta / dist;

        TargetInfo ti{};
        ti.type           = TargetingType::SKILLSHOT;
        ti.targetEntity   = static_cast<EntityID>(entt::to_integral(target));
        ti.targetPosition = targetTc->position;
        ti.direction      = dir;

        abilities.enqueueRequest(entity, AbilitySlot::Q, ti);
    }
}

} // namespace glory
