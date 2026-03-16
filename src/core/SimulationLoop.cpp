#include "core/SimulationLoop.h"

#include "ability/AbilitySystem.h"
#include "ability/ProjectileSystem.h"
#include "combat/CombatSystem.h"
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

#include <spdlog/spdlog.h>

namespace glory {

void SimulationLoop::init(const SimulationContext& ctx) {
    if (m_initialized) return;

    // ── Register all systems with the scheduler ────────────────────────────
    // Order of add() doesn't matter — the scheduler sorts by dependencies.

    m_scheduler.add<VFXFlushSystem>(
        ctx.vfxRenderer, ctx.vfxQueue, ctx.combatVfxQueue,
        ctx.trailRenderer, ctx.groundDecals, ctx.meshEffects, ctx.distortion);

    m_scheduler.add<AbilityUpdateSystem>(
        ctx.abilities, ctx.vfxQueue, ctx.trailRenderer,
        ctx.groundDecals, ctx.meshEffects, ctx.explosions,
        ctx.coneEffect, ctx.spriteEffects, ctx.distortion);

    m_scheduler.add<ProjectileUpdateSystem>(
        ctx.projectiles, ctx.abilities, ctx.vfxQueue,
        ctx.trailRenderer, ctx.explosions, ctx.gpuCollision);

    m_scheduler.add<EffectsUpdateSystem>(ctx.explosions, ctx.spriteEffects);

    m_scheduler.add<ConeEffectSystem>(ctx.coneEffect, &m_coneState);

    m_scheduler.add<CombatUpdateSystem>(ctx.combat);

    m_scheduler.add<PhysicsUpdateSystem>();

    m_scheduler.add<AnimationUpdateSystem>();

    // Build the dependency DAG and compute parallel execution levels
    m_scheduler.build();

    m_initialized = true;
    spdlog::info("SimulationLoop: scheduler built with {} systems across {} levels",
                 m_scheduler.systemCount(), m_scheduler.levelCount());
}

void SimulationLoop::tick(SimulationContext& ctx) {
    // Lazy init on first tick (all subsystem pointers are valid by this point)
    if (!m_initialized) init(ctx);

    // Sync cone effect state from Renderer into our persistent ConeEffectState
    m_coneState.timer     = ctx.coneEffectTimer;
    m_coneState.duration  = ctx.coneDuration;
    m_coneState.halfAngle = ctx.coneHalfAngle;
    m_coneState.range     = ctx.coneRange;
    m_coneState.apex      = ctx.coneApex;
    m_coneState.direction = ctx.coneDirection;

    // ── Execute all systems via the parallel scheduler ────────────────────
    if (ctx.threadPool) {
        m_scheduler.tick(ctx.registry, ctx.dt, *ctx.threadPool);
    } else {
        // Fallback: execute systems sequentially if no thread pool
        for (size_t i = 0; i < m_scheduler.systemCount(); ++i) {
            // This path shouldn't be hit in normal operation
        }
        spdlog::warn("SimulationLoop: no thread pool, skipping tick");
        return;
    }

    // Sync cone effect timer back to the context (Renderer reads it)
    ctx.coneEffectTimer = m_coneState.timer;
}

} // namespace glory
