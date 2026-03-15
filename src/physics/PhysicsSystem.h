#pragma once

#include <entt.hpp>
#include <glm/glm.hpp>

namespace glory {

// Physics integration + sleep/wake system for RigidBodyComponent entities.
//
// Correct call order per fixed-timestep tick:
//   1. PhysicsSystem::integrate(reg, dt)            — gravity, velocity, position
//   2. PhysicsSystem::resolveCollisionsAndWake(reg)  — iterative contact solver + wake
//   3. PhysicsSystem::updateSleep(reg, dt)           — sleep check AFTER bodies settle
//
// Separating updateSleep into step 3 ensures the iterative solver fully
// diffuses penetration energy before the epsilon thresholds are evaluated,
// preventing false early sleep caused by solver-introduced velocities.
class PhysicsSystem {
public:
    static constexpr float kSleepDelay = 0.5f;   // seconds below threshold → sleep
    static constexpr float kGravityY   = -9.81f;

    // Number of positional-correction iterations per contact manifold.
    // Higher values converge faster at the cost of CPU time.
    // 8 is a good default for stable stacking without visible popping.
    static int solverIterations; // default: 8 (defined in .cpp)

    // 1. Advance all awake RigidBodies: gravity, semi-implicit Euler,
    //    exponential damping. Does NOT check sleep — call updateSleep() after.
    static void integrate(entt::registry& reg, float dt);

    // 2. Sphere-sphere narrow phase.
    //    - Wakes any sleeping body struck by an active body.
    //    - Applies restitution velocity impulse (single pass).
    //    - Applies progressive positional correction over solverIterations passes:
    //        strength = (iter+1) / solverIterations  (eases penetration out smoothly).
    static void resolveCollisionsAndWake(entt::registry& reg);

    // 3. Evaluate sleep condition for all awake bodies. Run AFTER
    //    resolveCollisionsAndWake so solver-settled bodies reach the epsilon zone.
    static void updateSleep(entt::registry& reg, float dt);

    // ── Programmatic wake / force / impulse API ───────────────────────────────

    // Immediately wake a sleeping body (e.g. because nearby geometry changed).
    static void wake(entt::registry& reg, entt::entity entity);

    // Apply an instantaneous impulse and wake the body.
    static void applyImpulse(entt::registry& reg, entt::entity entity,
                             const glm::vec3& impulse);

    // Queue a continuous force for the next integration step and wake the body.
    static void applyForce(entt::registry& reg, entt::entity entity,
                           const glm::vec3& force);

    // Wake all dynamic bodies within radius of a world position.
    // Use this when static geometry is moved/destroyed.
    static void wakeInRadius(entt::registry& reg,
                             const glm::vec3& origin, float radius);
};

} // namespace glory
