#include "core/SimulationLoop.h"
// All system types are forward-declared in GameSystems.h (via SimulationLoop.h).
// Concrete includes needed for preTick / postTick subsystem calls.
#include "scene/Components.h"
#include "nav/DebugRenderer.h"
#include "audio/AudioEngine.h"
#include "audio/AudioResourceManager.h"
#include "terrain/IsometricCamera.h"
#include "vfx/VFXFactory.h"
#include "scripting/ScriptEngine.h"
#include "combat/GpuCollisionSystem.h"
#include "nav/DynamicObstacle.h"
#include "nav/PathfindingSystem.h"
#include "renderer/FogOfWarRenderer.h"
#include "fog/FogSystem.h"
#include "fog/FogOfWarGameplay.h"
#include "combat/CombatComponents.h"

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

    m_scheduler.add<EconomyUpdateSystem>(ctx.economy, ctx.gameTime);

    m_scheduler.add<StructureUpdateSystem>(ctx.structures);

    m_scheduler.add<MinionWaveUpdateSystem>(ctx.minionWaves, ctx.gameTime);

    m_scheduler.add<NPCBehaviorUpdateSystem>(ctx.npcBehavior, ctx.abilities);

    m_scheduler.add<RespawnUpdateSystem>(ctx.respawn);

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

    // Override dt with the deterministic fixed timestep
    ctx.dt = SimulationContext::FIXED_DT_FLOAT;

    // Expose the deterministic PRNG to all systems via the context
    ctx.rng = &m_rng;

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
        m_scheduler.tickSequential(ctx.registry, ctx.dt);
    }

    // Sync cone effect timer back to the context (Renderer reads it)
    ctx.coneEffectTimer = m_coneState.timer;

    ++m_tick;
}

void SimulationLoop::step(SimulationContext& ctx) {
    const float dt = ctx.dt;  // save before tick() overrides to FIXED_DT_FLOAT

    // ── Pre-tick: housekeeping that must run before the ECS systems ──────

    // Clear debug draws at start of each physics step
    if (ctx.debugRenderer) ctx.debugRenderer->clear();

    // Save previous transforms for interpolation
    {
        auto view = ctx.registry.view<TransformComponent>();
        for (auto [e, t] : view.each()) {
            t.prevPosition = t.position;
            t.prevRotation = t.rotation;
        }
    }

    // Advance game clock
    if (ctx.gameTime) *ctx.gameTime += dt;

    // Audio listener follows camera
    if (ctx.audioEngine && ctx.isoCam) {
        glm::vec3 camPos = ctx.isoCam->getPosition();
        glm::vec3 camFwd = glm::normalize(ctx.isoCam->getTarget() - camPos);
        ctx.audioEngine->setListenerPosition(camPos, camFwd, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    if (ctx.audioResources) ctx.audioResources->tick();

    // VFX definition hot-reload (checks filesystem timestamps)
    if (ctx.vfxFactory) ctx.vfxFactory->tickHotReload();

    // Lua script hot-reload
    if (ctx.scriptEngine) ctx.scriptEngine->reloadModified();

    // GPU collision: read back previous frame's results
    if (ctx.gpuCollision)
        const_cast<GpuCollisionSystem*>(ctx.gpuCollision)->readResults(ctx.currentFrame);

    // Navigation: update obstacles + regenerate dirty flow fields
    if (ctx.dynamicObstacles && ctx.pathfinding) {
        ctx.dynamicObstacles->update(dt);
        ctx.pathfinding->updateFlowFields(ctx.dynamicObstacles);
    }

    // ── Core simulation tick (ECS systems) ───────────────────────────────
    tick(ctx);

    // ── Post-tick: fog of war vision update ──────────────────────────────
    if (ctx.fogOfWar && ctx.fogSystem && ctx.fowGameplay) {
        ctx.fowGameplay->update(ctx.registry, *ctx.fogSystem, dt, Team::PLAYER);
        ctx.fogOfWar->updateVisibility(
            ctx.fogSystem->getVisibilityBuffer().data(), 128, 128);
    }
}

} // namespace glory
