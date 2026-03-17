#include "core/GameSystems.h"

#include "ability/AbilitySystem.h"
#include "ability/ProjectileSystem.h"
#include "combat/CombatSystem.h"
#include "combat/EconomySystem.h"
#include "combat/StructureSystem.h"
#include "combat/MinionWaveSystem.h"
#include "combat/RespawnSystem.h"
#include "physics/PhysicsSystem.h"
#include "vfx/VFXRenderer.h"
#include "vfx/VFXEventQueue.h"
#include "vfx/TrailRenderer.h"
#include "vfx/MeshEffectRenderer.h"
#include "renderer/GroundDecalRenderer.h"
#include "renderer/DistortionRenderer.h"
#include "renderer/ExplosionRenderer.h"
#include "renderer/ConeAbilityRenderer.h"
#include "renderer/SpriteEffectRenderer.h"
#include "scene/Components.h"

namespace glory {

// ═══════════════════════════════════════════════════════════════════════════════
// VFXFlushSystem
// ═══════════════════════════════════════════════════════════════════════════════
void VFXFlushSystem::execute(entt::registry& /*registry*/, float dt) {
    if (m_vfx) {
        if (m_q1) m_vfx->processQueue(*m_q1);
        if (m_q2) m_vfx->processQueue(*m_q2);
        m_vfx->update(dt);
    }
    if (m_trail)      m_trail->update(dt);
    if (m_decals)     m_decals->update(dt);
    if (m_mesh)       m_mesh->update(dt);
    if (m_distortion) m_distortion->update(dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// AbilityUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void AbilityUpdateSystem::execute(entt::registry& registry, float dt) {
    if (!m_abilities) return;

    m_abilities->update(registry, dt, m_trail);

    if (m_q && m_trail && m_decals && m_mesh &&
        m_explosions && m_cone && m_sprites && m_distortion)
    {
        m_abilities->getSequencer().update(dt,
            *m_q, *m_trail, *m_decals, *m_mesh,
            *m_explosions, *m_cone, *m_sprites, *m_distortion);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ProjectileUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void ProjectileUpdateSystem::execute(entt::registry& registry, float dt) {
    if (!m_proj || !m_abilities || !m_q) return;

    m_proj->update(registry, dt, *m_q, *m_abilities, m_trail, m_gpuCollision);

    if (m_explosions) {
        for (const auto& pos : m_proj->getLandedPositions()) {
            m_explosions->addExplosion(pos);
        }
        m_proj->clearLandedPositions();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// EffectsUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void EffectsUpdateSystem::execute(entt::registry& /*registry*/, float dt) {
    if (m_explosions) m_explosions->update(dt);
    if (m_sprites)    m_sprites->update(dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ConeEffectSystem
// ═══════════════════════════════════════════════════════════════════════════════
void ConeEffectSystem::execute(entt::registry& /*registry*/, float dt) {
    if (!m_state || m_state->timer <= 0.0f || !m_cone) return;

    m_state->timer -= dt;
    float coneElapsed = m_state->duration - m_state->timer;
    m_cone->update(dt, m_state->apex, m_state->direction,
                   m_state->halfAngle, m_state->range, coneElapsed);
}

// ═══════════════════════════════════════════════════════════════════════════════
// CombatUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void CombatUpdateSystem::execute(entt::registry& registry, float dt) {
    if (m_combat) m_combat->update(registry, dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PhysicsUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void PhysicsUpdateSystem::execute(entt::registry& registry, float dt) {
    PhysicsSystem::integrate(registry, dt);
    PhysicsSystem::resolveCollisionsAndWake(registry);
    PhysicsSystem::updateSleep(registry, dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// AnimationUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void AnimationUpdateSystem::execute(entt::registry& registry, float dt) {
    auto view = registry.view<AnimationComponent>();
    for (auto [entity, anim] : view.each()) {
        anim.player.update(dt);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// EconomyUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void EconomyUpdateSystem::execute(entt::registry& registry, float dt) {
    if (m_econ && m_gameTime)
        m_econ->update(registry, *m_gameTime, dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// StructureUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void StructureUpdateSystem::execute(entt::registry& registry, float dt) {
    if (m_structures) m_structures->update(registry, dt);
}

// ═══════════════════════════════════════════════════════════════════════════════
// MinionWaveUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void MinionWaveUpdateSystem::execute(entt::registry& registry, float dt) {
    if (m_waves && m_gameTime)
        m_waves->update(registry, dt, *m_gameTime);
}

// ═══════════════════════════════════════════════════════════════════════════════
// RespawnUpdateSystem
// ═══════════════════════════════════════════════════════════════════════════════
void RespawnUpdateSystem::execute(entt::registry& registry, float dt) {
    if (m_respawn) m_respawn->update(registry, dt);
}

} // namespace glory
