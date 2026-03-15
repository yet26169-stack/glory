#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace glory {

// Rigid body component — attach alongside TransformComponent to make an entity
// participate in physics simulation (gravity, collision, sleep/wake).
//
// Static bodies (isStatic = true) never move but DO participate in collision
// resolution as immovable objects.  Set mass = 0 / isStatic = true for walls,
// floors, and other level geometry.
struct RigidBodyComponent {
    // ── Linear state ─────────────────────────────────────────────────────────
    glm::vec3 linearVelocity{0.0f};
    glm::vec3 angularVelocity{0.0f};

    // Accumulated forces/torques applied this step; cleared after integration.
    glm::vec3 accumulatedForce{0.0f};
    glm::vec3 accumulatedTorque{0.0f};

    // ── Physical properties ───────────────────────────────────────────────────
    float mass            = 1.0f;
    float restitution     = 0.2f;  // bounciness coefficient
    float linearDamping   = 0.05f; // per-second velocity decay [0, 1)
    float angularDamping  = 0.05f;
    float collisionRadius = 0.5f;  // sphere approximation for broad/narrow phase

    // True for immovable level geometry — skips integration entirely.
    bool isStatic = false;

    // ── Sleep state ───────────────────────────────────────────────────────────
    float sleepTimer               = 0.0f;
    bool  isSleeping               = false;
    float linearVelocityThreshold  = 0.01f; // below this → candidate for sleep
    float angularVelocityThreshold = 0.01f;

    // ── Helpers ───────────────────────────────────────────────────────────────
    float inverseMass() const {
        return (isStatic || mass <= 0.0f) ? 0.0f : 1.0f / mass;
    }

    // Wake the body immediately (resets sleep timer and clears isSleeping flag).
    // No-op for static bodies.
    void wake() {
        if (isStatic) return;
        isSleeping = false;
        sleepTimer = 0.0f;
    }

    // Apply an instantaneous impulse (ΔV = impulse / mass) and wake.
    void applyImpulse(const glm::vec3& impulse) {
        if (isStatic) return;
        linearVelocity += impulse * inverseMass();
        wake();
    }

    // Queue a force for the next integration step and wake.
    void addForce(const glm::vec3& force) {
        if (isStatic) return;
        accumulatedForce += force;
        wake();
    }

    // Queue a torque for the next integration step and wake.
    void addTorque(const glm::vec3& torque) {
        if (isStatic) return;
        accumulatedTorque += torque;
        wake();
    }
};

} // namespace glory
