#include "physics/PhysicsEngine.h"
#include <spdlog/spdlog.h>

namespace glory {

PhysicsEngine::PhysicsEngine() = default;
PhysicsEngine::~PhysicsEngine() { shutdown(); }

void PhysicsEngine::init(const glm::vec3& gravity) {
    m_gravity = gravity;
    m_initialized = true;
    spdlog::info("Physics engine initialized (stub) — gravity ({}, {}, {})",
                 gravity.x, gravity.y, gravity.z);
}

void PhysicsEngine::shutdown() {
    if (!m_initialized) return;
    m_initialized = false;
    spdlog::info("Physics engine shut down");
}

void PhysicsEngine::step(float) {}

uint32_t PhysicsEngine::createBody(BodyType, const CollisionShape&,
                                    const glm::vec3&, float) { return 0; }
void PhysicsEngine::destroyBody(uint32_t) {}
void PhysicsEngine::setBodyPosition(uint32_t, const glm::vec3&) {}
glm::vec3 PhysicsEngine::getBodyPosition(uint32_t) const { return glm::vec3(0.0f); }
glm::quat PhysicsEngine::getBodyRotation(uint32_t) const { return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); }
void PhysicsEngine::applyForce(uint32_t, const glm::vec3&) {}
void PhysicsEngine::applyImpulse(uint32_t, const glm::vec3&) {}

PhysicsEngine::RayHit PhysicsEngine::raycast(const glm::vec3&, const glm::vec3&, float) const {
    return RayHit{};
}

} // namespace glory
