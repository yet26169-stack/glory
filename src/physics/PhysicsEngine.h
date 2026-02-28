#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>

namespace glory {

class PhysicsEngine {
public:
    PhysicsEngine();
    ~PhysicsEngine();

    PhysicsEngine(const PhysicsEngine&)            = delete;
    PhysicsEngine& operator=(const PhysicsEngine&) = delete;

    void init(const glm::vec3& gravity = glm::vec3(0.0f, -9.81f, 0.0f));
    void shutdown();
    void step(float deltaTime);

    enum class BodyType { Static, Dynamic, Kinematic };

    struct CollisionShape {
        enum class Type { Box, Sphere, Capsule, Mesh } type = Type::Box;
        glm::vec3 halfExtents{0.5f};
        float     radius = 0.5f;
        float     height = 1.0f;
    };

    uint32_t createBody(BodyType type, const CollisionShape& shape,
                        const glm::vec3& position, float mass = 1.0f);
    void     destroyBody(uint32_t bodyId);
    void     setBodyPosition(uint32_t bodyId, const glm::vec3& pos);
    glm::vec3 getBodyPosition(uint32_t bodyId) const;
    glm::quat getBodyRotation(uint32_t bodyId) const;
    void applyForce(uint32_t bodyId, const glm::vec3& force);
    void applyImpulse(uint32_t bodyId, const glm::vec3& impulse);

    struct RayHit {
        bool      hit = false;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f};
        uint32_t  bodyId = 0;
        float     distance = 0.0f;
    };

    RayHit raycast(const glm::vec3& origin, const glm::vec3& direction,
                   float maxDistance = 1000.0f) const;

    bool isInitialized() const { return m_initialized; }

private:
    bool      m_initialized = false;
    glm::vec3 m_gravity{0.0f, -9.81f, 0.0f};
};

struct RigidBodyComponent {
    uint32_t bodyId = 0;
    PhysicsEngine::BodyType type = PhysicsEngine::BodyType::Dynamic;
    float mass = 1.0f;
};

struct ColliderComponent {
    PhysicsEngine::CollisionShape shape;
};

} // namespace glory
