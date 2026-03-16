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

namespace glory {

void SimulationLoop::tick(SimulationContext& ctx) {
    const float dt = ctx.dt;

    // ── 1. Flush VFX event queues (game-thread → render-thread bridge) ───
    if (ctx.vfxRenderer) {
        if (ctx.vfxQueue)       ctx.vfxRenderer->processQueue(*ctx.vfxQueue);
        if (ctx.combatVfxQueue) ctx.vfxRenderer->processQueue(*ctx.combatVfxQueue);
        ctx.vfxRenderer->update(dt);
    }

    // ── 2. Update VFX subsystems ─────────────────────────────────────────
    if (ctx.trailRenderer)   ctx.trailRenderer->update(dt);
    if (ctx.groundDecals)    ctx.groundDecals->update(dt);
    if (ctx.meshEffects)     ctx.meshEffects->update(dt);
    if (ctx.distortion)      ctx.distortion->update(dt);

    // ── 3. Ability system (state machine, sequencer) ─────────────────────
    if (ctx.abilities) {
        ctx.abilities->update(ctx.registry, dt, ctx.trailRenderer);

        if (ctx.vfxQueue && ctx.trailRenderer && ctx.groundDecals &&
            ctx.meshEffects && ctx.explosions && ctx.coneEffect &&
            ctx.spriteEffects && ctx.distortion)
        {
            ctx.abilities->getSequencer().update(dt,
                *ctx.vfxQueue,
                *ctx.trailRenderer,
                *ctx.groundDecals,
                *ctx.meshEffects,
                *ctx.explosions,
                *ctx.coneEffect,
                *ctx.spriteEffects,
                *ctx.distortion);
        }
    }

    // ── 4. Projectile system ─────────────────────────────────────────────
    if (ctx.projectiles && ctx.abilities && ctx.vfxQueue) {
        ctx.projectiles->update(ctx.registry, dt,
                                *ctx.vfxQueue, *ctx.abilities, ctx.trailRenderer,
                                ctx.gpuCollision);
        if (ctx.explosions) {
            for (const auto& pos : ctx.projectiles->getLandedPositions()) {
                ctx.explosions->addExplosion(pos);
            }
            ctx.projectiles->clearLandedPositions();
        }
    }

    // ── 5. Explosion / sprite updates ────────────────────────────────────
    if (ctx.explosions)     ctx.explosions->update(dt);
    if (ctx.spriteEffects)  ctx.spriteEffects->update(dt);

    // ── 6. Cone ability effect tick ──────────────────────────────────────
    if (ctx.coneEffectTimer > 0.0f && ctx.coneEffect) {
        ctx.coneEffectTimer -= dt;
        float coneElapsed = ctx.coneDuration - ctx.coneEffectTimer;
        ctx.coneEffect->update(dt, ctx.coneApex, ctx.coneDirection,
                               ctx.coneHalfAngle, ctx.coneRange, coneElapsed);
    }

    // ── 7. Combat system (auto-attacks, shields, tricks) ─────────────────
    if (ctx.combat) {
        ctx.combat->update(ctx.registry, dt);
    }

    // ── 8. Rigid-body physics ─────────────────────────────────────────────
    // Step order matters for correctness:
    //   a) integrate:              gravity + semi-implicit Euler (no sleep check yet)
    //   b) resolveCollisionsAndWake: iterative progressive positional correction
    //   c) updateSleep:            sleep check AFTER solver settles bodies
    PhysicsSystem::integrate(ctx.registry, dt);
    PhysicsSystem::resolveCollisionsAndWake(ctx.registry);
    PhysicsSystem::updateSleep(ctx.registry, dt);
}

} // namespace glory
