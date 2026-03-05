#pragma once
#include "scene/Scene.h"
#include "ability/ProjectileSystem.h"
#include "minion/MinionSystem.h"   // HeightQueryFn is defined here
#include "structure/StructureSystem.h"
#include "jungle/JungleSystem.h"
#include "combat/AutoAttackSystem.h"
#include "renderer/ParticleSystem.h"
#include <functional>
#include <cstdint>

namespace glory {

class InputManager;

struct SimulationContext {
    Scene*             scene            = nullptr;
    InputManager*      input            = nullptr;
    ProjectileSystem*  projectileSystem = nullptr;
    MinionSystem*      minionSystem     = nullptr;
    StructureSystem*   structureSystem  = nullptr;
    JungleSystem*      jungleSystem     = nullptr;
    AutoAttackSystem*  autoAttackSystem = nullptr;
    ParticleSystem*    particles        = nullptr;
    HeightQueryFn      heightFn;
    float*             gameTime         = nullptr;
    uint32_t           currentFrame     = 0;
    bool               mobaMode         = true;
    bool               customMap        = false;
    /// Called once per simulation tick for renderer-private bookkeeping
    /// (bone slot cleanup, render component assignment to new entities).
    std::function<void()> postTickCallback;
};

class SimulationLoop {
public:
    static constexpr float FIXED_DT  = 1.0f / 30.0f;
    static constexpr float MAX_DELTA = 0.25f;
    static constexpr int   MAX_STEPS = 8;

    /// Advance the simulation by deltaTime seconds (may run 0..MAX_STEPS ticks).
    void tick(SimulationContext& ctx, float deltaTime);

    float getAccumulator() const { return m_accumulator; }
    /// Interpolation factor within the current tick [0, 1) for render blending.
    float getAlpha()       const { return m_alpha; }

private:
    float m_accumulator = 0.0f;
    float m_alpha       = 0.0f;
};

} // namespace glory
