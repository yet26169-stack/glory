#include "StructureSystem.h"
#include "StructureComponents.h"
#include "scene/Components.h"
#include <spdlog/spdlog.h>

namespace glory {

void StructureSystem::init(const StructureConfig &config, const MapData &mapData,
                           entt::registry &registry, HeightQueryFn heightFn) {
    m_config = config;
    for (int t = 0; t < 2; ++t) {
        m_nexus[t] = entt::null;
        for (int l = 0; l < 3; ++l) {
            m_inhibitors[t][l] = entt::null;
            for (int ti = 0; ti < 4; ++ti) {
                m_towers[t][l][ti] = entt::null;
            }
        }
    }
    
    spawnTowers(registry, mapData, heightFn);
    spawnInhibitors(registry, mapData, heightFn);
    spawnNexus(registry, mapData, heightFn);
    wireProtectionDependencies(registry);
}

void StructureSystem::update(entt::registry &registry, float dt, float gameTime) {
    if (m_gameOver) return;
    
    updateBackdoorProtection(registry);
    updateTargeting(registry, dt);
    updateCombat(registry, dt);
    updateProjectiles(registry, dt);
    updateInhibitorRespawns(registry, dt);
    updateNexusRegen(registry, dt);
    updatePlates(registry, gameTime);
    processDeaths(registry);
}

std::vector<StructureDeathEvent> StructureSystem::consumeDeathEvents() {
    auto events = m_deathEvents;
    m_deathEvents.clear();
    return events;
}

bool StructureSystem::isInhibitorDead(TeamID team, LaneType lane) const {
    if (team == TeamID::Neutral || lane == LaneType::Count) return false;
    auto e = m_inhibitors[static_cast<int>(team)][static_cast<int>(lane)];
    if (e == entt::null) return true; // if not spawned, consider dead
    // Need registry to check health, but typically we could just track state.
    // For now returning true if null. 
    return false; // Real implementation needs registry reference
}

entt::entity StructureSystem::getTower(TeamID team, LaneType lane, TowerTier tier) const {
    if (team == TeamID::Neutral || lane == LaneType::Count) return entt::null;
    return m_towers[static_cast<int>(team)][static_cast<int>(lane)][static_cast<int>(tier)];
}

void StructureSystem::spawnTowers(entt::registry &registry, const MapData &mapData,
                                  HeightQueryFn heightFn) {
    for (int t = 0; t < 2; ++t) {
        TeamID team = static_cast<TeamID>(t);
        for (const auto& towerDef : mapData.teams[t].towers) {
            auto e = registry.create();
            glm::vec3 pos = towerDef.position;
            if (t == 1 && towerDef.team2Override.has_value()) {
                pos = towerDef.team2Override.value();
            }
            if (heightFn) pos.y = heightFn(pos.x, pos.z);
            
            registry.emplace<TransformComponent>(e, TransformComponent{pos, {0,0,0}, {1.5f, 4.0f, 1.5f}});
            registry.emplace<TowerTag>(e);
            registry.emplace<StructureIdentityComponent>(e, StructureIdentityComponent{team, towerDef.lane, towerDef.tier});
            
            const auto& tCfg = m_config.towers[static_cast<int>(towerDef.tier)];
            registry.emplace<StructureHealthComponent>(e, StructureHealthComponent{tCfg.maxHP, tCfg.maxHP, 0.0f, 0.0f, 8.0f, false, false});
            registry.emplace<TowerAttackComponent>(e, TowerAttackComponent{tCfg.attackDamage, tCfg.attackRange, tCfg.attackCooldown, 0.0f, 0.0f, m_config.damageRampRate, m_config.damageRampMax, entt::null, tCfg.projectileSpeed});
            registry.emplace<BackdoorProtectionComponent>(e, BackdoorProtectionComponent{tCfg.backdoorReduction, 20.0f, true});
            registry.emplace<ProtectionDependencyComponent>(e);
            
            if (towerDef.tier == TowerTier::Outer) {
                registry.emplace<TowerPlateComponent>(e, TowerPlateComponent{
                    static_cast<uint8_t>(tCfg.plateCount),
                    tCfg.maxHP / tCfg.plateCount,
                    tCfg.goldPerPlate,
                    tCfg.armorPerPlate,
                    tCfg.plateFalloffTime
                });
            }
            
            m_towers[t][static_cast<int>(towerDef.lane)][static_cast<int>(towerDef.tier)] = e;
        }
    }
}

void StructureSystem::spawnInhibitors(entt::registry &registry, const MapData &mapData,
                                      HeightQueryFn heightFn) {
    for (int t = 0; t < 2; ++t) {
        TeamID team = static_cast<TeamID>(t);
        for (const auto& inhibDef : mapData.teams[t].inhibitors) {
            auto e = registry.create();
            glm::vec3 pos = inhibDef.position;
            if (t == 1 && inhibDef.team2Override.has_value()) {
                pos = inhibDef.team2Override.value();
            }
            if (heightFn) pos.y = heightFn(pos.x, pos.z);
            
            registry.emplace<TransformComponent>(e, TransformComponent{pos, {0,0,0}, {2.0f, 1.0f, 2.0f}});
            registry.emplace<InhibitorTag>(e);
            registry.emplace<StructureIdentityComponent>(e, StructureIdentityComponent{team, inhibDef.lane, TowerTier::Inhibitor});
            
            const auto& iCfg = m_config.inhibitor;
            registry.emplace<StructureHealthComponent>(e, StructureHealthComponent{iCfg.maxHP, iCfg.maxHP, 0.0f, 0.0f, 8.0f, false, false});
            registry.emplace<InhibitorComponent>(e, InhibitorComponent{0.0f, iCfg.respawnTime, false});
            registry.emplace<ProtectionDependencyComponent>(e);
            
            m_inhibitors[t][static_cast<int>(inhibDef.lane)] = e;
        }
    }
}

void StructureSystem::spawnNexus(entt::registry &registry, const MapData &mapData,
                                 HeightQueryFn heightFn) {
    for (int t = 0; t < 2; ++t) {
        TeamID team = static_cast<TeamID>(t);
        auto e = registry.create();
        glm::vec3 pos = mapData.teams[t].base.nexusPosition;
        if (heightFn) pos.y = heightFn(pos.x, pos.z);
        
        registry.emplace<TransformComponent>(e, TransformComponent{pos, {0,0,0}, {3.0f, 3.0f, 3.0f}});
        registry.emplace<NexusTag>(e);
        registry.emplace<StructureIdentityComponent>(e, StructureIdentityComponent{team, LaneType::Mid, TowerTier::Nexus}); // Mid lane doesn't matter much here
        
        const auto& nCfg = m_config.nexus;
        registry.emplace<StructureHealthComponent>(e, StructureHealthComponent{nCfg.maxHP, nCfg.maxHP, nCfg.hpRegen, 0.0f, nCfg.outOfCombatThreshold, false, false});
        registry.emplace<NexusComponent>(e, NexusComponent{nCfg.hpRegen});
        registry.emplace<ProtectionDependencyComponent>(e);
        
        m_nexus[t] = e;
    }
}

void StructureSystem::wireProtectionDependencies(entt::registry &registry) {
    for (int t = 0; t < 2; ++t) {
        for (int l = 0; l < 3; ++l) {
            auto outer = m_towers[t][l][static_cast<int>(TowerTier::Outer)];
            auto inner = m_towers[t][l][static_cast<int>(TowerTier::Inner)];
            auto inhibT = m_towers[t][l][static_cast<int>(TowerTier::Inhibitor)];
            auto inhib = m_inhibitors[t][l];
            
            if (registry.valid(inner)) {
                registry.get<ProtectionDependencyComponent>(inner).prerequisite = outer;
                registry.get<StructureHealthComponent>(inner).isInvulnerable = registry.valid(outer);
            }
            if (registry.valid(inhibT)) {
                registry.get<ProtectionDependencyComponent>(inhibT).prerequisite = inner;
                registry.get<StructureHealthComponent>(inhibT).isInvulnerable = registry.valid(inner);
            }
            if (registry.valid(inhib)) {
                registry.get<ProtectionDependencyComponent>(inhib).prerequisite = inhibT;
                registry.get<StructureHealthComponent>(inhib).isInvulnerable = registry.valid(inhibT);
            }
        }
        
        auto nex = m_nexus[t];
        auto nexT0 = m_towers[t][0][static_cast<int>(TowerTier::Nexus)]; // Assuming nexus towers are tracked somehow. The plan is a bit vague here.
        // Simplified nexus dependency: needs all inhibitors dead? Usually nexus towers need at least 1 inhibitor dead.
        if (registry.valid(nex)) {
            // we'll leave it without direct dependency for now and handle it dynamically
        }
    }
}

void StructureSystem::updateTargeting(entt::registry &registry, float dt) {
    auto view = registry.view<TowerTag, TransformComponent, TowerAttackComponent, StructureIdentityComponent, StructureHealthComponent>();
    for (auto entity : view) {
        auto [tT, tA, sI, sH] = view.get<TransformComponent, TowerAttackComponent, StructureIdentityComponent, StructureHealthComponent>(entity);
        if (sH.isDead) continue;
        
        // Simple mock targeting: just find any entity within range
        if (!registry.valid(tA.currentTarget)) {
            auto charView = registry.view<TransformComponent>(); // Should be Character or Minion
            float closestDist = tA.attackRange;
            for (auto target : charView) {
                if (target == entity) continue;
                auto& targetT = charView.get<TransformComponent>(target);
                float dist = glm::distance(tT.position, targetT.position);
                if (dist < closestDist) {
                    closestDist = dist;
                    tA.currentTarget = target;
                }
            }
        } else {
            // Check if current target is out of range or dead
            if (registry.valid(tA.currentTarget)) {
                auto& targetT = registry.get<TransformComponent>(tA.currentTarget);
                if (glm::distance(tT.position, targetT.position) > tA.attackRange) {
                    tA.currentTarget = entt::null;
                }
            }
        }
    }
}

void StructureSystem::updateCombat(entt::registry &registry, float dt) {
    auto view = registry.view<TowerAttackComponent, TransformComponent, StructureHealthComponent>();
    for (auto entity : view) {
        auto [tA, tT, sH] = view.get<TowerAttackComponent, TransformComponent, StructureHealthComponent>(entity);
        if (sH.isDead) continue;

        tA.timeSinceLastAttack += dt;
        if (tA.timeSinceLastAttack >= tA.attackCooldown && registry.valid(tA.currentTarget)) {
            tA.timeSinceLastAttack = 0.0f;
            
            // Spawn projectile
            auto projEntity = registry.create();
            registry.emplace<TransformComponent>(projEntity, TransformComponent{tT.position + glm::vec3(0, 3.0f, 0), {0,0,0}, {0.3f, 0.3f, 0.3f}});
            registry.emplace<TowerProjectileComponent>(projEntity, TowerProjectileComponent{entity, tA.currentTarget, tA.projectileSpeed, tA.attackDamage, 0.0f, 3.0f});
        }
    }
}

void StructureSystem::updateProjectiles(entt::registry &registry, float dt) {
    auto view = registry.view<TowerProjectileComponent, TransformComponent>();
    for (auto entity : view) {
        auto& proj = view.get<TowerProjectileComponent>(entity);
        auto& pos = view.get<TransformComponent>(entity).position;
        
        proj.age += dt;
        if (proj.age > proj.maxAge) {
            registry.destroy(entity);
            continue;
        }

        if (registry.valid(proj.target) && registry.all_of<TransformComponent>(proj.target)) {
            auto& targetPos = registry.get<TransformComponent>(proj.target).position;
            glm::vec3 dir = targetPos - pos;
            float dist = glm::length(dir);
            
            if (dist < 0.5f) {
                // hit
                // Apply damage (need health component on target)
                registry.destroy(entity);
            } else {
                pos += glm::normalize(dir) * proj.speed * dt;
            }
        } else {
            registry.destroy(entity);
        }
    }
}

void StructureSystem::updateBackdoorProtection(entt::registry &registry) {
    // mock
}

void StructureSystem::updateInhibitorRespawns(entt::registry &registry, float dt) {
    auto view = registry.view<InhibitorComponent, StructureHealthComponent>();
    for (auto entity : view) {
        auto [inhib, sH] = view.get<InhibitorComponent, StructureHealthComponent>(entity);
        if (sH.isDead && inhib.isRespawning) {
            inhib.respawnTimer -= dt;
            if (inhib.respawnTimer <= 0.0f) {
                sH.isDead = false;
                sH.currentHP = sH.maxHP;
                inhib.isRespawning = false;
            }
        }
    }
}

void StructureSystem::updateNexusRegen(entt::registry &registry, float dt) {
    auto view = registry.view<NexusComponent, StructureHealthComponent>();
    for (auto entity : view) {
        auto [nex, sH] = view.get<NexusComponent, StructureHealthComponent>(entity);
        if (!sH.isDead && sH.currentHP < sH.maxHP && sH.outOfCombatTime > sH.outOfCombatThreshold) {
            sH.currentHP = std::min(sH.maxHP, sH.currentHP + nex.hpRegen * dt);
        }
    }
}

void StructureSystem::updatePlates(entt::registry &registry, float gameTime) {
    // mock
}

void StructureSystem::processDeaths(entt::registry &registry) {
    auto view = registry.view<StructureHealthComponent, StructureIdentityComponent, TransformComponent>();
    for (auto entity : view) {
        auto& sH = view.get<StructureHealthComponent>(entity);
        if (sH.currentHP <= 0.0f && !sH.isDead) {
            sH.isDead = true;
            auto& sI = view.get<StructureIdentityComponent>(entity);
            auto& t = view.get<TransformComponent>(entity);
            
            StructureDeathEvent event;
            event.entity = entity;
            event.team = sI.team;
            event.lane = sI.lane;
            event.tier = sI.tier;
            event.position = t.position;
            
            if (registry.all_of<TowerTag>(entity)) event.type = EntityType::Tower;
            else if (registry.all_of<InhibitorTag>(entity)) {
                event.type = EntityType::Inhibitor;
                registry.get<InhibitorComponent>(entity).isRespawning = true;
                registry.get<InhibitorComponent>(entity).respawnTimer = registry.get<InhibitorComponent>(entity).respawnTime;
            }
            else if (registry.all_of<NexusTag>(entity)) {
                event.type = EntityType::Nexus;
                m_gameOver = true;
                m_winningTeam = (sI.team == TeamID::Blue) ? TeamID::Red : TeamID::Blue;
            }
            
            m_deathEvents.push_back(event);
            
            // Update dependencies
            auto depView = registry.view<ProtectionDependencyComponent, StructureHealthComponent>();
            for (auto depEnt : depView) {
                auto& dep = depView.get<ProtectionDependencyComponent>(depEnt);
                if (dep.prerequisite == entity) {
                    depView.get<StructureHealthComponent>(depEnt).isInvulnerable = false;
                }
            }
        }
    }
}

} // namespace glory
