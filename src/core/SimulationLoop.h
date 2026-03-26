#pragma once

#include "core/SystemScheduler.h"
#include "core/GameSystems.h"
#include "core/FixedPoint.h"
#include "core/DeterministicRNG.h"
#include "vfx/VFXEventQueue.h"

#include <entt.hpp>
#include <glm/glm.hpp>

#include <memory>

namespace glory {

class AbilitySystem;
class GpuCollisionSystem;
class ProjectileSystem;
class CombatSystem;
class EconomySystem;
class StructureSystem;
class MinionWaveSystem;
class RespawnSystem;
class NPCBehaviorSystem;
class VFXRenderer;
class TrailRenderer;
class GroundDecalRenderer;
class DeferredDecalRenderer;
class MeshEffectRenderer;
class DistortionRenderer;
class ExplosionRenderer;
class ConeAbilityRenderer;
class SpriteEffectRenderer;
class ThreadPool;
class DebugRenderer;
class AudioEngine;
class AudioResourceManager;
class IsometricCamera;
class VFXFactory;
class ScriptEngine;
class DynamicObstacleManager;
class PathfindingSystem;
class FogOfWarRenderer;
class FogSystem;
class FogOfWarGameplay;

// All state needed by the simulation loop, passed by reference from the Renderer.
struct SimulationContext {
    entt::registry& registry;
    float dt;

    // Deterministic fixed timestep (1/30 s = Fixed32(1)/Fixed32(30))
    static constexpr float TICK_RATE_HZ  = 30.0f;
    static constexpr float FIXED_DT_FLOAT = 1.0f / TICK_RATE_HZ;

    // Deterministic PRNG — shared across all gameplay systems.
    // Seeded once at game start; state is included in snapshots for rollback.
    DeterministicRNG* rng = nullptr;

    // Game systems (non-owning references)
    AbilitySystem*        abilities       = nullptr;
    ProjectileSystem*     projectiles     = nullptr;
    CombatSystem*         combat          = nullptr;
    EconomySystem*        economy         = nullptr;
    StructureSystem*      structures      = nullptr;
    MinionWaveSystem*     minionWaves     = nullptr;
    RespawnSystem*        respawn         = nullptr;
    NPCBehaviorSystem*    npcBehavior     = nullptr;
    const GpuCollisionSystem* gpuCollision = nullptr;

    // VFX subsystems (non-owning references)
    VFXRenderer*          vfxRenderer     = nullptr;
    VFXEventQueue*        vfxQueue        = nullptr;
    VFXEventQueue*        combatVfxQueue  = nullptr;
    TrailRenderer*        trailRenderer   = nullptr;
    GroundDecalRenderer*  groundDecals    = nullptr;
    DeferredDecalRenderer* deferredDecals = nullptr;
    MeshEffectRenderer*   meshEffects     = nullptr;
    DistortionRenderer*   distortion      = nullptr;
    ExplosionRenderer*    explosions      = nullptr;
    ConeAbilityRenderer*  coneEffect      = nullptr;
    SpriteEffectRenderer* spriteEffects   = nullptr;

    // Cone effect state (owned by Renderer, passed in for update)
    float  coneEffectTimer  = 0.0f;
    float  coneDuration     = 0.0f;
    float  coneHalfAngle    = 0.0f;
    float  coneRange        = 0.0f;
    glm::vec3 coneApex      = glm::vec3(0.0f);
    glm::vec3 coneDirection = glm::vec3(0.0f, 0.0f, 1.0f);

    // Radial blur state (sync-back from ability system → renderer)
    glm::vec3 radialBlurWorldCenter = glm::vec3(0.0f);
    float     radialBlurDuration    = 0.0f;
    float     radialBlurPeak        = 0.0f;
    bool      radialBlurTriggered   = false;

    // Thread pool for parallel system execution
    ThreadPool* threadPool = nullptr;

    // Game time for economy passive income
    float* gameTime = nullptr;

    // ── Pre-tick / post-tick subsystems (driven by SimulationLoop::step) ──
    DebugRenderer*          debugRenderer      = nullptr;
    AudioEngine*            audioEngine        = nullptr;
    AudioResourceManager*   audioResources     = nullptr;
    IsometricCamera*        isoCam             = nullptr;
    VFXFactory*             vfxFactory         = nullptr;
    ScriptEngine*           scriptEngine       = nullptr;
    DynamicObstacleManager* dynamicObstacles   = nullptr;
    PathfindingSystem*      pathfinding        = nullptr;
    FogOfWarRenderer*       fogOfWar           = nullptr;
    FogSystem*              fogSystem          = nullptr;
    FogOfWarGameplay*       fowGameplay        = nullptr;
    uint32_t                currentFrame       = 0;
};

// Runs all gameplay/simulation updates decoupled from the render loop.
// This makes it straightforward to:
//   1. Run simulation at a fixed timestep (30 Hz) independent of frame rate
//   2. Snapshot/restore state for networking rollback
//   3. Test simulation logic without a GPU
class SimulationLoop {
public:
    // Build the system scheduler from the subsystem pointers in a context.
    // Must be called once after all subsystems are initialized.
    void init(const SimulationContext& ctx);

    // Advance the simulation by one fixed timestep tick.
    // Uses the SystemScheduler for dependency-declared parallel execution.
    void tick(SimulationContext& ctx);

    // Full simulation step: preTick (transform save, audio, hot-reload,
    // navigation, GPU collision) → tick → postTick (FoW).
    // Renderer::simulateStep() delegates here for everything except input/HUD.
    void step(SimulationContext& ctx);

    SystemScheduler&       scheduler()       { return m_scheduler; }
    const SystemScheduler& scheduler() const { return m_scheduler; }

    ConeEffectState& coneState() { return m_coneState; }

    /// Deterministic PRNG owned by the simulation — seed once, snapshot for rollback.
    DeterministicRNG&       rng()       { return m_rng; }
    const DeterministicRNG& rng() const { return m_rng; }

    /// Seed the deterministic PRNG (call at game start or replay load).
    void seedRNG(uint64_t seed) { m_rng.seed(seed); }

    /// Current simulation tick counter.
    uint32_t currentTick() const { return m_tick; }

private:
    SystemScheduler  m_scheduler;
    ConeEffectState  m_coneState;
    DeterministicRNG m_rng;
    uint32_t         m_tick = 0;
    bool             m_initialized = false;
};

} // namespace glory
