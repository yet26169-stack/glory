#include "combat/MinionWaveSystem.h"
#include "combat/CombatComponents.h"
#include "combat/EconomySystem.h"
#include "ability/AbilityComponents.h"
#include "nav/PathfindingSystem.h"
#include "scene/Scene.h"
#include "scene/Components.h"

#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace glory {

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
    reg.emplace<TagComponent>(e, TagComponent{name});

    // Transform
    auto& tc = reg.emplace<TransformComponent>(e);
    tc.position = pos;
    tc.scale    = glm::vec3(0.05f); // match existing minion scale
    tc.rotation = glm::vec3(0.0f);

    // Team
    Team teamEnum = (team == 0) ? Team::PLAYER : Team::ENEMY;
    reg.emplace<TeamComponent>(e, TeamComponent{teamEnum});

    // Tint for team-coloured rendering
    reg.emplace<TintComponent>(e, TintComponent{tint});

    // Stats
    StatsComponent sc;
    sc.base.maxHP        = stats.hp;
    sc.base.currentHP    = stats.hp;
    sc.base.armor        = 10.0f;
    sc.base.magicResist  = 10.0f;
    sc.base.attackDamage = stats.ad;
    reg.emplace<StatsComponent>(e, sc);

    // Combat
    CombatComponent cc;
    cc.attackRange  = stats.attackRange;
    cc.attackSpeed  = 0.8f;
    cc.attackDamage = stats.ad;
    cc.isRanged     = stats.isRanged;
    if (stats.isRanged) cc.projectileSpeed = 15.0f;
    reg.emplace<CombatComponent>(e, cc);

    // Minion economy (kill rewards)
    reg.emplace<MinionComponent>(e, MinionComponent{stats.type, stats.goldReward, stats.xpReward});
    reg.emplace<EconomyComponent>(e, EconomyComponent{0, 0, 1}); // minions start with 0 gold

    // Movement
    reg.emplace<UnitComponent>(e, UnitComponent{UnitComponent::State::MOVING, pos, stats.moveSpeed});
    reg.emplace<CharacterComponent>(e, CharacterComponent{pos, stats.moveSpeed});
    reg.emplace<SelectableComponent>(e, SelectableComponent{false, 1.0f});

    // Wave AI component
    WaveMinionComponent wmc;
    wmc.teamIndex = team;
    wmc.laneIndex = lane;
    wmc.type      = stats.type;
    reg.emplace<WaveMinionComponent>(e, wmc);

    // Mesh + material + animation (from shared spawn config)
    reg.emplace<GPUSkinnedMeshComponent>(e, GPUSkinnedMeshComponent{m_spawnCfg.meshIndex});
    reg.emplace<MaterialComponent>(e, MaterialComponent{
        m_spawnCfg.texIndex, m_spawnCfg.flatNormIndex, 0.0f, 0.0f, 0.5f, 0.2f});

    // Skeleton + animation
    SkeletonComponent skelComp;
    skelComp.skeleton         = m_spawnCfg.skeleton;
    skelComp.skinVertices     = m_spawnCfg.skinVertices;
    skelComp.bindPoseVertices = m_spawnCfg.bindPoseVertices;

    AnimationComponent animComp;
    animComp.player.setSkeleton(&skelComp.skeleton);
    animComp.clips = m_spawnCfg.clips;
    if (!animComp.clips.empty()) {
        animComp.activeClipIndex = 0;
        animComp.player.setClip(&animComp.clips[0]);
    }

    reg.emplace<SkeletonComponent>(e, std::move(skelComp));
    reg.emplace<AnimationComponent>(e, std::move(animComp));

    return e;
}

// ═════════════════════════════════════════════════════════════════════════════
// Movement + aggro AI each tick
// ═════════════════════════════════════════════════════════════════════════════
void MinionWaveSystem::updateMovementAndAggro(entt::registry& reg, float dt) {
    auto view = reg.view<WaveMinionComponent, TransformComponent,
                         CharacterComponent, StatsComponent, CombatComponent>();

    for (auto&& [entity, wmc, transform, character, stats, combat]
         : view.each()) {

        // Skip dead minions
        if (stats.base.currentHP <= 0.0f) continue;

        glm::vec3 pos = transform.position;

        // ── Hero aggro timer countdown ──────────────────────────────────────
        if (wmc.heroAggroTimer > 0.0f) {
            wmc.heroAggroTimer -= dt;
            if (wmc.heroAggroTimer <= 0.0f) {
                wmc.heroAggroTarget = entt::null;
                wmc.heroAggroTimer = 0.0f;
            }
        }

        // ── Validate current hero aggro target ──────────────────────────────
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

        // ── Find target (hero aggro overrides normal aggro) ─────────────────
        entt::entity target = entt::null;
        if (wmc.heroAggroTarget != entt::null) {
            target = wmc.heroAggroTarget;
        } else {
            target = findTarget(reg, entity, wmc, pos);
        }
        wmc.aggroTarget = target;
        combat.targetEntity = target;

        // ── Move toward target or follow flow field ─────────────────────────
        if (target != entt::null) {
            // Move toward aggro target
            auto& targetTransform = reg.get<TransformComponent>(target);
            glm::vec3 toTarget = targetTransform.position - pos;
            float dist = glm::length(toTarget);

            if (dist > combat.attackRange) {
                // Move toward target
                glm::vec3 dir = toTarget / dist;
                transform.position += dir * character.moveSpeed * dt;
                transform.position.y = 0.0f;

                // Update facing
                character.currentYaw = std::atan2(dir.x, dir.z);
                character.hasTarget = true;
                character.targetPosition = targetTransform.position;
            }
            // Else: in attack range, CombatSystem handles the actual attack
        } else {
            // No target: follow flow field along the lane
            if (m_pathfinding && m_pathfinding->flowFieldsReady()) {
                glm::vec2 dir2 = m_pathfinding->sampleFlowField(
                    wmc.teamIndex, wmc.laneIndex,
                    glm::vec2(pos.x, pos.z));

                if (glm::length(dir2) > 0.01f) {
                    glm::vec3 dir3(dir2.x, 0.0f, dir2.y);
                    transform.position += dir3 * character.moveSpeed * dt;
                    transform.position.y = 0.0f;

                    character.currentYaw = std::atan2(dir3.x, dir3.z);
                    character.hasTarget = true;
                    character.targetPosition = pos + dir3 * 5.0f;
                }
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Find best target: enemy minions/structures within aggro range
// ═════════════════════════════════════════════════════════════════════════════
entt::entity MinionWaveSystem::findTarget(entt::registry& reg,
                                           entt::entity self,
                                           const WaveMinionComponent& wmc,
                                           glm::vec3 selfPos) const {
    Team myTeam = (wmc.teamIndex == 0) ? Team::PLAYER : Team::ENEMY;
    float bestDist = wmc.aggroRange;
    entt::entity best = entt::null;

    // Check enemy minions and heroes (anything with TeamComponent + StatsComponent)
    auto targets = reg.view<TransformComponent, TeamComponent, StatsComponent>();
    for (auto&& [e, tt, teamComp, tStats] : targets.each()) {
        if (e == self) continue;
        if (teamComp.team == myTeam || teamComp.team == Team::NEUTRAL) continue;
        if (tStats.base.currentHP <= 0.0f) continue;

        float dist = glm::length(tt.position - selfPos);
        if (dist < bestDist) {
            bestDist = dist;
            best = e;
        }
    }

    return best;
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

} // namespace glory
