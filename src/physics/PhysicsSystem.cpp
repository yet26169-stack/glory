#include "physics/PhysicsSystem.h"
#include "core/Profiler.h"

#include "physics/RigidBodyComponent.h"
#include "scene/Components.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace glory {

// Configurable solver iteration count — change at runtime via PhysicsSystem::solverIterations.
int PhysicsSystem::solverIterations = 8;

static constexpr glm::vec3 kGravity{0.0f, PhysicsSystem::kGravityY, 0.0f};

// Baumgarte constants (shared by both single-pass impulse and iterative correction).
static constexpr float kSlop = 0.005f; // penetration depth below which no correction
static constexpr float kBeta = 0.2f;   // base correction fraction per pass

// ── integrate ─────────────────────────────────────────────────────────────────
// Applies gravity, integrates velocity/position, clears accumulated forces.
// Sleep evaluation is intentionally NOT done here — call updateSleep() after
// resolveCollisionsAndWake() so the iterative solver finishes before we check
// whether a body has settled below the epsilon thresholds.

void PhysicsSystem::integrate(entt::registry& reg, float dt) {
    GLORY_ZONE_N("PhysicsIntegrate");
    auto view = reg.view<TransformComponent, RigidBodyComponent>();

    for (auto [entity, transform, rb] : view.each()) {
        if (rb.isStatic)   continue;
        if (rb.isSleeping) continue; // skip gravity and integration for sleeping bodies

        rb.accumulatedForce += kGravity * rb.mass;

        float invMass = rb.inverseMass();
        rb.linearVelocity  += rb.accumulatedForce  * invMass * dt;
        rb.angularVelocity += rb.accumulatedTorque * invMass * dt;

        rb.linearVelocity  *= std::max(0.0f, 1.0f - rb.linearDamping  * dt);
        rb.angularVelocity *= std::max(0.0f, 1.0f - rb.angularDamping * dt);

        transform.position += rb.linearVelocity  * dt;
        transform.rotation += rb.angularVelocity * dt;

        rb.accumulatedForce  = {0.0f, 0.0f, 0.0f};
        rb.accumulatedTorque = {0.0f, 0.0f, 0.0f};
    }
}

// ── resolveCollisionsAndWake ──────────────────────────────────────────────────
// Phase 1 — broad+narrow: build contact manifold, apply velocity impulses,
//           wake any sleeping body struck by an active body.
// Phase 2 — iterative positional correction: solverIterations passes with
//           progressively increasing strength to ease penetration out smoothly
//           without the energy pop of a single large correction.

void PhysicsSystem::resolveCollisionsAndWake(entt::registry& reg) {
    GLORY_ZONE_N("PhysicsResolve");
    // ── Collect dynamic entities ─────────────────────────────────────────────
    struct Entry {
        glm::vec3*          pos;
        RigidBodyComponent* rb;
    };
    std::vector<Entry> dynamic;
    dynamic.reserve(64);

    auto view = reg.view<TransformComponent, RigidBodyComponent>();
    for (auto [e, t, rb] : view.each()) {
        if (!rb.isStatic) dynamic.push_back({ &t.position, &rb });
    }

    // ── Contact manifold ─────────────────────────────────────────────────────
    struct Contact {
        glm::vec3*          posA;
        glm::vec3*          posB;
        RigidBodyComponent* rbA;
        RigidBodyComponent* rbB;
        glm::vec3           normal;       // unit vector from A toward B at first contact
        float               combinedR;    // sum of collision radii
    };
    std::vector<Contact> contacts;
    contacts.reserve(32);

    // ── Phase 1: broad+narrow phase → impulse + manifold build ──────────────
    for (size_t i = 0; i < dynamic.size(); ++i) {
        auto& A = dynamic[i];
        if (A.rb->isSleeping) continue; // sleeping cannot initiate contact

        for (size_t j = i + 1; j < dynamic.size(); ++j) {
            auto& B = dynamic[j];

            float combinedR = A.rb->collisionRadius + B.rb->collisionRadius;
            glm::vec3 delta = *B.pos - *A.pos;
            float dx = delta.x, dy = delta.y, dz = delta.z;
            float dist2 = dx*dx + dy*dy + dz*dz;

            if (dist2 >= combinedR * combinedR) continue; // no overlap

            // Wake sleeping partner immediately on first contact
            if (B.rb->isSleeping) B.rb->wake();

            float dist = std::sqrt(dist2);
            if (dist < 1e-6f) continue;

            glm::vec3 normal = delta / dist;

            // Velocity impulse — single pass (velocity is not iterative here;
            // only the positional correction is iterated).
            glm::vec3 relVel    = A.rb->linearVelocity - B.rb->linearVelocity;
            float     velAlongN = glm::dot(relVel, normal);
            if (velAlongN < 0.0f) {
                float e         = std::min(A.rb->restitution, B.rb->restitution);
                float invA      = A.rb->inverseMass();
                float invB      = B.rb->inverseMass();
                float totalInvM = invA + invB;
                if (totalInvM > 0.0f) {
                    float j = -(1.0f + e) * velAlongN / totalInvM;
                    A.rb->linearVelocity -= j * invA * normal;
                    B.rb->linearVelocity += j * invB * normal;
                    A.rb->wake();
                    B.rb->wake();
                }
            }

            contacts.push_back({ A.pos, B.pos, A.rb, B.rb, normal, combinedR });
        }
    }

    if (contacts.empty()) return;

    // ── Phase 2: iterative progressive positional correction ─────────────────
    // Each iteration re-evaluates the CURRENT penetration depth (which shrinks
    // as positions are corrected), then applies a progressively stronger fraction
    // of the remaining overlap.  This eases objects out of penetration without
    // the instantaneous pop of a single large correction.
    //
    //   strength = (iter + 1) / solverIterations
    //     iter 0: 1/8 of full correction  (gentle nudge)
    //     iter 7: 8/8 of full correction  (final cleanup)
    //
    // Because penetration decreases each iteration, the actual correction per
    // pass diminishes naturally — the total converges without overshooting.
    for (int iter = 0; iter < solverIterations; ++iter) {
        float strength = static_cast<float>(iter + 1) /
                         static_cast<float>(solverIterations);

        for (auto& c : contacts) {
            // Recompute separation from current (already partially corrected) positions.
            glm::vec3 d  = *c.posB - *c.posA;
            float dx = d.x, dy = d.y, dz = d.z;
            float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

            float penetration = c.combinedR - dist;
            if (penetration <= kSlop) continue; // within tolerance — no correction needed

            float invA      = c.rbA->inverseMass();
            float invB      = c.rbB->inverseMass();
            float totalInvM = invA + invB;
            if (totalInvM <= 0.0f) continue;

            // Use current contact normal; fallback to cached normal if coincident.
            glm::vec3 n = (dist > 1e-6f) ? (d / dist) : c.normal;

            glm::vec3 correction = n * (kBeta * strength * (penetration - kSlop)
                                        / totalInvM);
            *c.posA -= correction * invA;
            *c.posB += correction * invB;
        }
    }
}

// ── updateSleep ───────────────────────────────────────────────────────────────
// Run AFTER resolveCollisionsAndWake so the solver has finished moving bodies
// before we evaluate whether they have settled below the sleep thresholds.

void PhysicsSystem::updateSleep(entt::registry& reg, float dt) {
    GLORY_ZONE_N("PhysicsSleep");
    auto view = reg.view<RigidBodyComponent>();
    for (auto [entity, rb] : view.each()) {
        if (rb.isStatic || rb.isSleeping) continue;

        float linSpeed = glm::length(rb.linearVelocity);
        float angSpeed = glm::length(rb.angularVelocity);

        if (linSpeed < rb.linearVelocityThreshold &&
            angSpeed < rb.angularVelocityThreshold) {
            rb.sleepTimer += dt;
            if (rb.sleepTimer >= kSleepDelay) {
                rb.isSleeping      = true;
                rb.linearVelocity  = {0.0f, 0.0f, 0.0f};
                rb.angularVelocity = {0.0f, 0.0f, 0.0f};
            }
        } else {
            rb.sleepTimer = 0.0f;
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void PhysicsSystem::wake(entt::registry& reg, entt::entity entity) {
    if (auto* rb = reg.try_get<RigidBodyComponent>(entity)) rb->wake();
}

void PhysicsSystem::applyImpulse(entt::registry& reg, entt::entity entity,
                                 const glm::vec3& impulse) {
    if (auto* rb = reg.try_get<RigidBodyComponent>(entity)) rb->applyImpulse(impulse);
}

void PhysicsSystem::applyForce(entt::registry& reg, entt::entity entity,
                               const glm::vec3& force) {
    if (auto* rb = reg.try_get<RigidBodyComponent>(entity)) rb->addForce(force);
}

void PhysicsSystem::wakeInRadius(entt::registry& reg,
                                 const glm::vec3& origin, float radius) {
    float r2 = radius * radius;
    auto view = reg.view<TransformComponent, RigidBodyComponent>();
    for (auto [entity, t, rb] : view.each()) {
        if (rb.isStatic || !rb.isSleeping) continue;
        glm::vec3 d = t.position - origin;
        if (d.x*d.x + d.y*d.y + d.z*d.z <= r2) rb.wake();
    }
}

} // namespace glory
