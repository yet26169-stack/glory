#include "ability/AbilityComponents.h"
#include "ability/AbilitySystem.h"
#include "ability/AbilityTypes.h"
#include "combat/CombatComponents.h"
#include "combat/EconomySystem.h"
#include "combat/MinionWaveSystem.h"
#include "combat/NPCBehaviorSystem.h"
#include "combat/RespawnSystem.h"
#include "fog/FogComponents.h"
#include "nav/PathfindingSystem.h"
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

    // Minions die permanently (no respawn, entity destroyed)
    reg.emplace<RespawnComponent>(e, RespawnComponent{
        LifeState::ALIVE, 0.f, 0.f, glm::vec3(0.f), false /*isHero=false*/
    });

    // FoW vision: minions have small sight radius
    reg.emplace<VisionComponent>(e, VisionComponent{6.0f});

    // Mesh + material + animation (from shared spawn config)
    reg.emplace<GPUSkinnedMeshComponent>(e, GPUSkinnedMeshComponent{m_spawnCfg.meshIndex});
    reg.emplace<MaterialComponent>(e, MaterialComponent{
        m_spawnCfg.texIndex, m_spawnCfg.flatNormIndex, 0.0f, 0.0f, 0.5f, 0.2f});

    // GPU skinning only needs the joint hierarchy (for matrix computation) — not raw vertex data.
    // Copying skinVertices/bindPoseVertices per entity caused ~53 MB/wave allocation overhead.
    SkeletonComponent skelComp;
    skelComp.skeleton = m_spawnCfg.skeleton;

    AnimationComponent animComp;
    animComp.player.setSkeleton(&skelComp.skeleton);
    animComp.clips = m_spawnCfg.clips;
    if (!animComp.clips.empty()) {
        animComp.activeClipIndex = 0;
        animComp.player.setClip(&animComp.clips[0]);
    }

    reg.emplace<SkeletonComponent>(e, std::move(skelComp));
    reg.emplace<AnimationComponent>(e, std::move(animComp));

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
            // Validate current cached target before doing a full search
            bool currentValid = false;
            if (wmc.aggroTarget != entt::null && reg.valid(wmc.aggroTarget)) {
                auto* tgt = reg.try_get<TransformComponent>(wmc.aggroTarget);
                auto* tgtStats = reg.try_get<StatsComponent>(wmc.aggroTarget);
                auto* tgtTeam = reg.try_get<TeamComponent>(wmc.aggroTarget);
                if (tgt && tgtStats && tgtTeam && tgtStats->base.currentHP > 0.0f) {
                    Team myTeam = (wmc.teamIndex == 0) ? Team::PLAYER : Team::ENEMY;
                    if (tgtTeam->team != myTeam && tgtTeam->team != Team::NEUTRAL) {
                        float dist = glm::length(tgt->position - pos);
                        if (dist <= wmc.aggroRange * 1.5f) {
                            currentValid = true;
                            target = wmc.aggroTarget;
                        }
                    }
                }
            }

            // Only do a full registry scan if we have no valid target and
            // the retarget cooldown has elapsed.
            if (!currentValid) {
                wmc.retargetCooldown -= dt;
                if (wmc.retargetCooldown <= 0.0f) {
                    target = findTarget(reg, entity, wmc, pos);
                    wmc.retargetCooldown = WaveMinionComponent::RETARGET_INTERVAL;
                }
            }
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
                transform.position.y = 0.2f;

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
                    transform.position.y = 0.2f;

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
