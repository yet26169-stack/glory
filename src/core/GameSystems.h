#pragma once

// ISystem adapters that wrap existing gameplay systems for the SystemScheduler.
// Each adapter captures non-owning pointers to the subsystems it needs,
// and delegates execute() to the real system's update method.

#include "core/SystemScheduler.h"
#include "vfx/VFXEventQueue.h"
#include "physics/PhysicsSystem.h"

#include <glm/glm.hpp>
#include <typeindex>

namespace glory {

// Forward declarations (avoid pulling in heavy headers)
class AbilitySystem;
class ProjectileSystem;
class CombatSystem;
class GpuCollisionSystem;
class VFXRenderer;
class TrailRenderer;
class GroundDecalRenderer;
class DeferredDecalRenderer;
class MeshEffectRenderer;
class DistortionRenderer;
class ExplosionRenderer;
class ConeAbilityRenderer;
class SpriteEffectRenderer;
class EconomySystem;
class StructureSystem;
class MinionWaveSystem;
class RespawnSystem;
class NPCBehaviorSystem;

// ═══════════════════════════════════════════════════════════════════════════════
// VFXFlushSystem — flushes VFX event queues and updates VFX subsystems
// Must run before systems that produce VFX events.
// ═══════════════════════════════════════════════════════════════════════════════
class VFXFlushSystem : public ISystem {
public:
    VFXFlushSystem(VFXRenderer* vfx, VFXEventQueue* q1, VFXEventQueue* q2,
                   TrailRenderer* trail, GroundDecalRenderer* decals,
                   DeferredDecalRenderer* deferredDecals,
                   MeshEffectRenderer* mesh, DistortionRenderer* distortion)
        : m_vfx(vfx), m_q1(q1), m_q2(q2), m_trail(trail)
        , m_decals(decals), m_deferredDecals(deferredDecals)
        , m_mesh(mesh), m_distortion(distortion) {}

    void execute(entt::registry& registry, float dt) override;
    std::string_view name() const override { return "VFXFlush"; }

private:
    VFXRenderer*          m_vfx;
    VFXEventQueue*        m_q1;
    VFXEventQueue*        m_q2;
    TrailRenderer*        m_trail;
    GroundDecalRenderer*  m_decals;
    DeferredDecalRenderer* m_deferredDecals;
    MeshEffectRenderer*   m_mesh;
    DistortionRenderer*   m_distortion;
};

// ═══════════════════════════════════════════════════════════════════════════════
// AbilityUpdateSystem — ability state machine + composite sequencer
// Depends on nothing (per prompt).
// ═══════════════════════════════════════════════════════════════════════════════
class AbilityUpdateSystem : public ISystem {
public:
    AbilityUpdateSystem(AbilitySystem* abilities, VFXEventQueue* q,
                        TrailRenderer* trail, GroundDecalRenderer* decals,
                        DeferredDecalRenderer* deferredDecals,
                        MeshEffectRenderer* mesh, ExplosionRenderer* explosions,
                        ConeAbilityRenderer* cone, SpriteEffectRenderer* sprites,
                        DistortionRenderer* distortion)
        : m_abilities(abilities), m_q(q), m_trail(trail), m_decals(decals)
        , m_deferredDecals(deferredDecals), m_mesh(mesh), m_explosions(explosions)
        , m_cone(cone), m_sprites(sprites), m_distortion(distortion) {}

    void execute(entt::registry& registry, float dt) override;
    std::vector<std::type_index> dependsOn() const override {
        return {}; // No formal dependencies
    }
    std::string_view name() const override { return "AbilityUpdate"; }

private:
    AbilitySystem*        m_abilities;
    VFXEventQueue*        m_q;
    TrailRenderer*        m_trail;
    GroundDecalRenderer*  m_decals;
    DeferredDecalRenderer* m_deferredDecals;
    MeshEffectRenderer*   m_mesh;
    ExplosionRenderer*    m_explosions;
    ConeAbilityRenderer*  m_cone;
    SpriteEffectRenderer* m_sprites;
    DistortionRenderer*   m_distortion;
};

// ═══════════════════════════════════════════════════════════════════════════════
// ProjectileUpdateSystem — moves projectiles, checks collisions, spawns VFX
// Depends on nothing (per prompt).
// ═══════════════════════════════════════════════════════════════════════════════
class ProjectileUpdateSystem : public ISystem {
public:
    ProjectileUpdateSystem(ProjectileSystem* proj, AbilitySystem* abilities,
                           VFXEventQueue* q, TrailRenderer* trail,
                           ExplosionRenderer* explosions,
                           const GpuCollisionSystem* gpuCollision)
        : m_proj(proj), m_abilities(abilities), m_q(q)
        , m_trail(trail), m_explosions(explosions)
        , m_gpuCollision(gpuCollision) {}

    void execute(entt::registry& registry, float dt) override;
    std::vector<std::type_index> dependsOn() const override {
        return { std::type_index(typeid(AbilityUpdateSystem)) };
    }
    std::string_view name() const override { return "ProjectileUpdate"; }

private:
    ProjectileSystem*         m_proj;
    AbilitySystem*            m_abilities;
    VFXEventQueue*            m_q;
    TrailRenderer*            m_trail;
    ExplosionRenderer*        m_explosions;
    const GpuCollisionSystem* m_gpuCollision;
};

// ═══════════════════════════════════════════════════════════════════════════════
// EffectsUpdateSystem — explosion and sprite effect ticks
// Depends on ProjectileUpdateSystem (consumes landed positions).
// ═══════════════════════════════════════════════════════════════════════════════
class EffectsUpdateSystem : public ISystem {
public:
    EffectsUpdateSystem(ExplosionRenderer* explosions, SpriteEffectRenderer* sprites)
        : m_explosions(explosions), m_sprites(sprites) {}

    void execute(entt::registry& registry, float dt) override;
    std::vector<std::type_index> dependsOn() const override {
        return { std::type_index(typeid(ProjectileUpdateSystem)) };
    }
    std::string_view name() const override { return "EffectsUpdate"; }

private:
    ExplosionRenderer*    m_explosions;
    SpriteEffectRenderer* m_sprites;
};

// ═══════════════════════════════════════════════════════════════════════════════
// ConeEffectSystem — W-ability cone visual tick
// ═══════════════════════════════════════════════════════════════════════════════
struct ConeEffectState {
    float  timer     = 0.0f;
    float  duration  = 0.0f;
    float  halfAngle = 0.0f;
    float  range     = 0.0f;
    glm::vec3 apex{0.0f};
    glm::vec3 direction{0.0f, 0.0f, 1.0f};
};

class ConeEffectSystem : public ISystem {
public:
    ConeEffectSystem(ConeAbilityRenderer* cone, ConeEffectState* state)
        : m_cone(cone), m_state(state) {}

    void execute(entt::registry& registry, float dt) override;
    std::string_view name() const override { return "ConeEffect"; }

private:
    ConeAbilityRenderer* m_cone;
    ConeEffectState*     m_state;
};

// ═══════════════════════════════════════════════════════════════════════════════
// CombatUpdateSystem — auto-attacks, shields, tricks
// Depends on nothing (per prompt).
// ═══════════════════════════════════════════════════════════════════════════════
class CombatUpdateSystem : public ISystem {
public:
    explicit CombatUpdateSystem(CombatSystem* combat) : m_combat(combat) {}

    void execute(entt::registry& registry, float dt) override;
    std::vector<std::type_index> dependsOn() const override {
        return { std::type_index(typeid(ProjectileUpdateSystem)) };
    }
    std::string_view name() const override { return "CombatUpdate"; }

private:
    CombatSystem* m_combat;
};

// ═══════════════════════════════════════════════════════════════════════════════
// PhysicsUpdateSystem — integrate, resolve collisions, update sleep
// Depends on nothing (per prompt). Runs all 3 physics sub-steps in order.
// ═══════════════════════════════════════════════════════════════════════════════
class PhysicsUpdateSystem : public ISystem {
public:
    PhysicsUpdateSystem() = default;

    void execute(entt::registry& registry, float dt) override;
    std::vector<std::type_index> dependsOn() const override {
        return { std::type_index(typeid(CombatUpdateSystem)) };
    }
    std::string_view name() const override { return "PhysicsUpdate"; }
};

// ═══════════════════════════════════════════════════════════════════════════════
// EconomyUpdateSystem — passive gold income, level-up checks
// Depends on CombatUpdateSystem (kill rewards happen during combat tick).
// ═══════════════════════════════════════════════════════════════════════════════
class EconomyUpdateSystem : public ISystem {
public:
    EconomyUpdateSystem(EconomySystem* econ, float* gameTime)
        : m_econ(econ), m_gameTime(gameTime) {}

    void execute(entt::registry& registry, float dt) override;
    std::vector<std::type_index> dependsOn() const override {
        return { std::type_index(typeid(CombatUpdateSystem)) };
    }
    std::string_view name() const override { return "EconomyUpdate"; }

private:
    EconomySystem* m_econ;
    float*         m_gameTime;
};

// ═══════════════════════════════════════════════════════════════════════════════
// StructureUpdateSystem — tower AI, inhibitor respawn, nexus death
// Depends on CombatUpdateSystem (needs fresh HP after combat tick).
// ═══════════════════════════════════════════════════════════════════════════════
class StructureUpdateSystem : public ISystem {
public:
    explicit StructureUpdateSystem(StructureSystem* structures)
        : m_structures(structures) {}

    void execute(entt::registry& registry, float dt) override;
    std::vector<std::type_index> dependsOn() const override {
        return { std::type_index(typeid(CombatUpdateSystem)) };
    }
    std::string_view name() const override { return "StructureUpdate"; }

private:
    StructureSystem* m_structures;
};

// ═══════════════════════════════════════════════════════════════════════════════
// RespawnUpdateSystem — detects unit death, runs respawn timers, destroys minions
// Depends on CombatUpdate + StructureUpdate (needs final HP after all damage).
// ═══════════════════════════════════════════════════════════════════════════════
class RespawnUpdateSystem : public ISystem {
public:
    explicit RespawnUpdateSystem(RespawnSystem* respawn)
        : m_respawn(respawn) {}

    void execute(entt::registry& registry, float dt) override;
    std::vector<std::type_index> dependsOn() const override {
        return { std::type_index(typeid(CombatUpdateSystem)),
                 std::type_index(typeid(StructureUpdateSystem)) };
    }
    std::string_view name() const override { return "RespawnUpdate"; }

private:
    RespawnSystem* m_respawn;
};

// ═══════════════════════════════════════════════════════════════════════════════
// MinionWaveUpdateSystem — spawns lane minion waves, handles wave AI + movement
// Must run after RespawnUpdate (dead entities orphaned before AI iteration).
// ═══════════════════════════════════════════════════════════════════════════════
class MinionWaveUpdateSystem : public ISystem {
public:
    MinionWaveUpdateSystem(MinionWaveSystem* waves, float* gameTime)
        : m_waves(waves), m_gameTime(gameTime) {}

    void execute(entt::registry& registry, float dt) override;
    std::vector<std::type_index> dependsOn() const override {
        return { std::type_index(typeid(CombatUpdateSystem)),
                 std::type_index(typeid(StructureUpdateSystem)),
                 std::type_index(typeid(RespawnUpdateSystem)) };
    }
    std::string_view name() const override { return "MinionWaveUpdate"; }

private:
    MinionWaveSystem* m_waves;
    float*            m_gameTime;
};

// ═══════════════════════════════════════════════════════════════════════════════
// AnimationUpdateSystem — ticks all AnimationPlayer components
// Depends on all movement + spawning systems so entity emplacement is complete.
// ═══════════════════════════════════════════════════════════════════════════════
class AnimationUpdateSystem : public ISystem {
public:
    AnimationUpdateSystem() = default;

    void execute(entt::registry& registry, float dt) override;
    std::vector<std::type_index> dependsOn() const override {
        return {
            std::type_index(typeid(PhysicsUpdateSystem)),
            std::type_index(typeid(CombatUpdateSystem)),
            std::type_index(typeid(ProjectileUpdateSystem)),
            std::type_index(typeid(MinionWaveUpdateSystem)),
            std::type_index(typeid(RespawnUpdateSystem)),
        };
    }
    std::string_view name() const override { return "AnimationUpdate"; }
};

// ═══════════════════════════════════════════════════════════════════════════════
// NPCBehaviorUpdateSystem — drives ability casting for wave minions.
// Must run AFTER MinionWaveUpdateSystem (aggroTarget already resolved).
// ═══════════════════════════════════════════════════════════════════════════════
class NPCBehaviorUpdateSystem : public ISystem {
public:
    NPCBehaviorUpdateSystem(NPCBehaviorSystem* npc, AbilitySystem* abilities)
        : m_npc(npc), m_abilities(abilities) {}

    void execute(entt::registry& registry, float dt) override;
    std::vector<std::type_index> dependsOn() const override {
        return { std::type_index(typeid(MinionWaveUpdateSystem)) };
    }
    std::string_view name() const override { return "NPCBehaviorUpdate"; }

private:
    NPCBehaviorSystem* m_npc;
    AbilitySystem*     m_abilities;
};

} // namespace glory

