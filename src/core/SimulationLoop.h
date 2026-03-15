#pragma once

#include "vfx/VFXEventQueue.h"

#include <entt.hpp>
#include <glm/glm.hpp>

#include <memory>

namespace glory {

class AbilitySystem;
class ProjectileSystem;
class CombatSystem;
class VFXRenderer;
class TrailRenderer;
class GroundDecalRenderer;
class MeshEffectRenderer;
class DistortionRenderer;
class ExplosionRenderer;
class ConeAbilityRenderer;
class SpriteEffectRenderer;

// All state needed by the simulation loop, passed by reference from the Renderer.
struct SimulationContext {
    entt::registry& registry;
    float dt;

    // Game systems (non-owning references)
    AbilitySystem*        abilities       = nullptr;
    ProjectileSystem*     projectiles     = nullptr;
    CombatSystem*         combat          = nullptr;

    // VFX subsystems (non-owning references)
    VFXRenderer*          vfxRenderer     = nullptr;
    VFXEventQueue*        vfxQueue        = nullptr;
    VFXEventQueue*        combatVfxQueue  = nullptr;
    TrailRenderer*        trailRenderer   = nullptr;
    GroundDecalRenderer*  groundDecals    = nullptr;
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
};

// Runs all gameplay/simulation updates decoupled from the render loop.
// This makes it straightforward to:
//   1. Run simulation at a fixed timestep (30 Hz) independent of frame rate
//   2. Snapshot/restore state for networking rollback
//   3. Test simulation logic without a GPU
class SimulationLoop {
public:
    // Advance the simulation by one frame's worth of delta-time.
    // Currently runs variable-step (matching render dt).
    // Future: accumulate dt and tick at FIXED_DT intervals.
    static void tick(SimulationContext& ctx);
};

} // namespace glory
