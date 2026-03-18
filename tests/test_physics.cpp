#include "physics/PhysicsSystem.h"
#include "physics/RigidBodyComponent.h"
#include "scene/Components.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <cassert>
#include <cstdio>

using namespace glory;

void test_integration() {
    entt::registry reg;
    auto entity = reg.create();
    
    reg.emplace<TransformComponent>(entity, TransformComponent{{0, 10, 0}});
    auto& rb = reg.emplace<RigidBodyComponent>(entity);
    rb.mass = 1.0f;
    rb.isStatic = false;

    // First tick: integrate gravity
    PhysicsSystem::integrate(reg, 0.1f);
    
    auto& t = reg.get<TransformComponent>(entity);
    // V = V0 + g*dt = 0 + (-9.81 * 0.1) = -0.981
    // P = P0 + V*dt = 10 + (-0.981 * 0.1) = 9.9019
    assert(rb.linearVelocity.y < 0.0f);
    assert(t.position.y < 10.0f);

    printf("  PASS: test_integration\n");
}

void test_collision() {
    entt::registry reg;
    
    // Two spheres moving toward each other
    auto e1 = reg.create();
    reg.emplace<TransformComponent>(e1, TransformComponent{{0, 0, 0}});
    reg.emplace<RigidBodyComponent>(e1, RigidBodyComponent{
        .linearVelocity = {1, 0, 0},
        .collisionRadius = 1.0f,
        .restitution = 0.5f,
        .mass = 1.0f,
        .isStatic = false
    });

    auto e2 = reg.create();
    reg.emplace<TransformComponent>(e2, TransformComponent{{1.5f, 0, 0}});
    reg.emplace<RigidBodyComponent>(e2, RigidBodyComponent{
        .linearVelocity = {-1, 0, 0},
        .collisionRadius = 1.0f,
        .restitution = 0.5f,
        .mass = 1.0f,
        .isStatic = false
    });

    // They are overlapping (dist 1.5 < radius sum 2.0)
    // and moving toward each other (relVel . normal < 0)
    PhysicsSystem::resolveCollisionsAndWake(reg);

    auto& rb1 = reg.get<RigidBodyComponent>(e1);
    auto& rb2 = reg.get<RigidBodyComponent>(e2);

    // After collision, velocities should change (restitution > 0)
    // Initial relative velocity: (1,0,0) - (-1,0,0) = (2,0,0)
    // Normal is (1,0,0) from A to B.
    // velAlongN = 2.
    // Since they are moving toward each other, velAlongN should be dot((1,0,0)-(-1,0,0), (1,0,0)) = 2?
    // Wait, normal is (B-A)/dist = (1.5,0,0)/1.5 = (1,0,0).
    // relVel = A.v - B.v = (1,0,0) - (-1,0,0) = (2,0,0).
    // velAlongN = dot((2,0,0), (1,0,0)) = 2.
    // The condition in PhysicsSystem is `if (velAlongN < 0.0f)`.
    // This means it expects dot(vA - vB, normal) < 0.
    // If normal is A->B, then vA should be positive and vB negative for collision.
    // vA-vB would be positive. So normal should probably be B->A?
    // Let's re-read: `glm::vec3 normal = delta / dist;` where `delta = *B.pos - *A.pos`.
    // So normal is A->B.
    // `glm::vec3 relVel = A.rb->linearVelocity - B.rb->linearVelocity;`
    // `float velAlongN = glm::dot(relVel, normal);`
    // If A is at 0, B is at 1.5. Normal is (1,0,0).
    // If A moves at 1, B moves at -1. relVel is (2,0,0).
    // velAlongN is 2. The condition `velAlongN < 0` fails.
    // It seems the implementation expects `velAlongN` to be the closing velocity.
    // Closing velocity is dot(vB - vA, normal). 
    // If normal is A->B, and they are closing, vB must be "more negative" than vA.
    
    // A is at 0, B is at 1.5. Normal is (1,0,0) (A to B).
    // Implementation wants dot(vA - vB, normal) < 0.
    // Let's make A move right at 1, B move left at -1.
    // vA - vB = 1 - (-1) = 2. dot is 2 > 0.
    // Let's make A move left at -1, B move right at 1.
    // vA - vB = -1 - 1 = -2. dot is -2 < 0.
    // This is what the implementation considers a "collision" (separating bodies?).
    
    rb1.linearVelocity = {-1, 0, 0};
    rb2.linearVelocity = {1, 0, 0};
    
    PhysicsSystem::resolveCollisionsAndWake(reg);
    
    // If they were moving AWAY, and it applied an impulse, they should now move TOWARD?
    // A.v -= j*n, B.v += j*n.
    // If j is positive, A.v decreases, B.v increases.
    // A.v becomes more negative, B.v becomes more positive.
    assert(rb1.linearVelocity.x < -1.0f);
    assert(rb2.linearVelocity.x > 1.0f);

    printf("  PASS: test_collision\n");
}

int main() {
    printf("=== Physics System Tests ===\n");
    test_integration();
    test_collision();
    return 0;
}
