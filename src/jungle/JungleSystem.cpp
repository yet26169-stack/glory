#include "JungleSystem.h"
#include "JungleComponents.h"
#include "scene/Components.h"

namespace glory {

void JungleSystem::init(const JungleConfig &config, const MapData &mapData,
                        entt::registry &registry, HeightQueryFn heightFn) {
    m_config = config;

    for (uint32_t i = 0; i < mapData.neutralCamps.size(); ++i) {
        const auto& campDef = mapData.neutralCamps[i];
        
        auto ctrlEntity = registry.create();
        registry.emplace<CampControllerComponent>(ctrlEntity, CampControllerComponent{
            i, campDef.campType, campDef.spawnTime, campDef.respawnTime, campDef.spawnTime, false, 0, 0, campDef.position
        });
        m_campControllers.push_back(ctrlEntity);
    }
}

void JungleSystem::update(entt::registry &registry, float dt, float gameTime,
                          HeightQueryFn heightFn) {
    updateSpawnTimers(registry, dt, gameTime, heightFn);
    updateAggro(registry, dt);
    updateStateTransitions(registry, dt);
    updateMovement(registry, dt, heightFn);
    updateCombat(registry, dt);
    updateReset(registry, dt);
    processDeaths(registry, gameTime);
}

std::vector<MonsterDeathEvent> JungleSystem::consumeDeathEvents() {
    auto events = m_deathEvents;
    m_deathEvents.clear();
    return events;
}

void JungleSystem::spawnCamp(entt::registry &registry, uint32_t campIndex,
                             const NeutralCamp &camp, HeightQueryFn heightFn) {
    if (m_config.camps.find(camp.campType) == m_config.camps.end()) return;
    const auto& cDef = m_config.camps[camp.campType];
    
    auto& ctrl = registry.get<CampControllerComponent>(m_campControllers[campIndex]);
    ctrl.isAlive = true;
    ctrl.mobCount = cDef.mobs.size();
    ctrl.aliveMobs = cDef.mobs.size();

    for (uint32_t i = 0; i < cDef.mobs.size(); ++i) {
        const auto& mDef = cDef.mobs[i];
        
        auto e = registry.create();
        glm::vec3 pos = camp.position;
        if (i < camp.mobPositions.size()) pos = camp.mobPositions[i];
        if (heightFn) pos.y = heightFn(pos.x, pos.z);
        
        float scale = mDef.isBig ? 1.0f : 0.6f;
        registry.emplace<TransformComponent>(e, TransformComponent{pos, {0,0,0}, {scale, scale, scale}});
        registry.emplace<JungleMonsterTag>(e);
        
        registry.emplace<MonsterIdentityComponent>(e, MonsterIdentityComponent{camp.campType, campIndex, i, mDef.isBig});
        registry.emplace<MonsterHealthComponent>(e, MonsterHealthComponent{mDef.hp, mDef.hp, false, entt::null});
        registry.emplace<MonsterCombatComponent>(e, MonsterCombatComponent{mDef.ad, mDef.armor, mDef.mr, mDef.range, mDef.cooldown, 0.0f});
        registry.emplace<MonsterAggroComponent>(e, MonsterAggroComponent{entt::null, 5.0f, camp.leashRadius, pos, 0.0f, 8.0f, false});
        registry.emplace<MonsterStateComponent>(e, MonsterStateComponent{MonsterState::Idle, 0.0f});
        registry.emplace<MonsterMovementComponent>(e, MonsterMovementComponent{3.0f, {0,0,0}});
        
        if (mDef.isBig && cDef.buff.duration > 0.0f) {
            registry.emplace<MonsterBuffComponent>(e, MonsterBuffComponent{cDef.buff.id, cDef.buff.duration});
        }
    }
}

void JungleSystem::updateSpawnTimers(entt::registry &registry, float dt, float gameTime,
                                     HeightQueryFn heightFn) {
    // Assume we need access to mapData for full camp info, but here we can just use the controller
    // For a real implementation we'd probably pass MapData to update or store NeutralCamps
    // For now this is mocked
}

void JungleSystem::updateAggro(entt::registry &registry, float dt) {
    auto view = registry.view<MonsterStateComponent, MonsterHealthComponent, MonsterAggroComponent, TransformComponent>();
    for (auto e : view) {
        auto [state, hp, aggro, t] = view.get<MonsterStateComponent, MonsterHealthComponent, MonsterAggroComponent, TransformComponent>(e);
        
        if (state.state == MonsterState::Idle) {
            if (hp.currentHP < hp.maxHP) {
                aggro.currentTarget = hp.lastAttacker;
                state.state = MonsterState::Attacking;
                state.stateTimer = 0.0f;
            } else {
                // Check proximity to champions
                auto charView = registry.view<TransformComponent>(); // Just any char for now
                for (auto ce : charView) {
                    if (e == ce) continue;
                    auto& ct = charView.get<TransformComponent>(ce);
                    if (glm::distance(t.position, ct.position) <= aggro.aggroRange) {
                        aggro.currentTarget = ce;
                        state.state = MonsterState::Attacking;
                        state.stateTimer = 0.0f;
                        break;
                    }
                }
            }
        }
    }
}

void JungleSystem::updateStateTransitions(entt::registry &registry, float dt) {
    auto view = registry.view<MonsterStateComponent, MonsterAggroComponent, TransformComponent>();
    for (auto e : view) {
        auto [state, aggro, t] = view.get<MonsterStateComponent, MonsterAggroComponent, TransformComponent>(e);
        
        if (state.state == MonsterState::Attacking || state.state == MonsterState::Chasing) {
            float distToHome = glm::distance(t.position, aggro.homePosition);
            if (distToHome > aggro.leashRadius) {
                aggro.patience += dt;
                if (aggro.patience >= aggro.patienceMax) {
                    state.state = MonsterState::Resetting;
                    aggro.currentTarget = entt::null;
                    aggro.isResetting = true;
                }
            } else {
                aggro.patience = std::max(0.0f, aggro.patience - dt);
            }
        }
    }
}

void JungleSystem::updateMovement(entt::registry &registry, float dt, HeightQueryFn heightFn) {
    auto view = registry.view<MonsterStateComponent, MonsterMovementComponent, TransformComponent, MonsterAggroComponent>();
    for (auto e : view) {
        auto [state, move, t, aggro] = view.get<MonsterStateComponent, MonsterMovementComponent, TransformComponent, MonsterAggroComponent>(e);
        
        if (state.state == MonsterState::Chasing && registry.valid(aggro.currentTarget)) {
            auto& targetT = registry.get<TransformComponent>(aggro.currentTarget);
            glm::vec3 dir = glm::normalize(targetT.position - t.position);
            t.position += dir * move.moveSpeed * dt;
        } else if (state.state == MonsterState::Resetting) {
            glm::vec3 dir = aggro.homePosition - t.position;
            float dist = glm::length(dir);
            if (dist < 0.5f) {
                t.position = aggro.homePosition;
                state.state = MonsterState::Idle;
                aggro.isResetting = false;
                aggro.patience = 0.0f;
            } else {
                t.position += glm::normalize(dir) * move.moveSpeed * dt;
            }
        }
        
        if (heightFn) t.position.y = heightFn(t.position.x, t.position.z);
    }
}

void JungleSystem::updateCombat(entt::registry &registry, float dt) {
    auto view = registry.view<MonsterStateComponent, MonsterCombatComponent, MonsterAggroComponent, TransformComponent>();
    for (auto e : view) {
        auto [state, combat, aggro, t] = view.get<MonsterStateComponent, MonsterCombatComponent, MonsterAggroComponent, TransformComponent>(e);
        
        if (state.state == MonsterState::Attacking) {
            combat.timeSinceLastAttack += dt;
            if (combat.timeSinceLastAttack >= combat.attackCooldown) {
                // mock attack
                combat.timeSinceLastAttack = 0.0f;
            }
        }
    }
}

void JungleSystem::updateReset(entt::registry &registry, float dt) {
    auto view = registry.view<MonsterStateComponent, MonsterHealthComponent>();
    for (auto e : view) {
        auto [state, hp] = view.get<MonsterStateComponent, MonsterHealthComponent>(e);
        if (state.state == MonsterState::Resetting) {
            hp.currentHP = std::min(hp.maxHP, hp.currentHP + hp.maxHP * 0.1f * dt); // heal fast
        }
    }
}

void JungleSystem::processDeaths(entt::registry &registry, float gameTime) {
    auto view = registry.view<MonsterHealthComponent, MonsterIdentityComponent, TransformComponent>();
    for (auto e : view) {
        auto& hp = view.get<MonsterHealthComponent>(e);
        if (hp.currentHP <= 0.0f && !hp.isDead) {
            hp.isDead = true;
            auto& id = view.get<MonsterIdentityComponent>(e);
            auto& t = view.get<TransformComponent>(e);
            
            MonsterDeathEvent event;
            event.entity = e;
            event.campType = id.campType;
            event.campIndex = id.campIndex;
            event.position = t.position;
            event.killer = hp.lastAttacker;
            
            const auto& campDef = m_config.camps[id.campType];
            event.goldReward = campDef.goldReward;
            event.xpReward = campDef.xpReward;
            
            if (id.isBigMonster && registry.all_of<MonsterBuffComponent>(e)) {
                auto& buff = registry.get<MonsterBuffComponent>(e);
                event.buffId = buff.buffId;
                event.buffDuration = buff.buffDuration;
            }
            
            m_deathEvents.push_back(event);
            
            auto& ctrl = registry.get<CampControllerComponent>(m_campControllers[id.campIndex]);
            ctrl.aliveMobs--;
            if (ctrl.aliveMobs == 0) {
                ctrl.isAlive = false;
                ctrl.respawnTimer = ctrl.respawnTime;
            }
        }
    }
}

} // namespace glory
