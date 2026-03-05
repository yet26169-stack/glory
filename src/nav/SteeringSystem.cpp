#include "nav/SteeringSystem.h"
#include <cmath>

namespace glory {

void SteeringSystem::update(entt::registry& reg,
                             const std::vector<FlowField*>& activeFields,
                             SimFloat dt) {
    auto view = reg.view<FlowFieldAgent, SimPosition, SimVelocity>();

    for (auto e : view) {
        auto& agent = view.get<FlowFieldAgent>(e);
        auto& pos   = view.get<SimPosition>(e);
        auto& vel   = view.get<SimVelocity>(e);

        // Find the flow field this agent is assigned to
        glm::vec2 flowDir{0.0f};
        for (FlowField* ff : activeFields) {
            if (ff && ff->id == agent.flowFieldID && ff->isBuilt()) {
#ifdef GLORY_DETERMINISTIC
                flowDir = ff->getDirection(pos.value.x.toFloat(), pos.value.z.toFloat());
#else
                flowDir = ff->getDirection(pos.value.x, pos.value.z);
#endif
                break;
            }
        }

        // Separation from neighbours
        SimVec3 sep = separation(reg, e, agent.separationRadius);

        // Combine: flow direction + separation
        const float speed = 5.0f; // units/s; TODO: per-entity speed component

#ifdef GLORY_DETERMINISTIC
        SimVec3 desired{
            Fixed64::fromFloat(flowDir.x * speed) + sep.x,
            Fixed64(0),
            Fixed64::fromFloat(flowDir.y * speed) + sep.z
        };
        vel.value = desired;
        pos.value = pos.value + vel.value * dt;
#else
        SimVec3 desired{
            flowDir.x * speed + sep.x,
            0.0f,
            flowDir.y * speed + sep.z
        };
        vel.value = desired;
        pos.value = pos.value + vel.value * dt;
#endif
    }
}

SimVec3 SteeringSystem::separation(entt::registry& reg, entt::entity self,
                                    SimFloat radius) {
    SimVec3 force{};
    auto& selfPos = reg.get<SimPosition>(self);

    auto view = reg.view<FlowFieldAgent, SimPosition>();
    for (auto other : view) {
        if (other == self) continue;
        auto& otherPos = view.get<SimPosition>(other);
        SimVec3 diff = selfPos.value - otherPos.value;

#ifdef GLORY_DETERMINISTIC
        Fixed64 dx = diff.x, dz = diff.z;
        Fixed64 distSq = dx*dx + dz*dz;
        Fixed64 rSq    = radius * radius;
        if (distSq < rSq && distSq.raw > 0) {
            Fixed64 dist = fixedSqrt(distSq);
            Fixed64 strength = (radius - dist) / radius;
            force.x = force.x + (dx / dist) * strength;
            force.z = force.z + (dz / dist) * strength;
        }
#else
        float dx = diff.x, dz = diff.z;
        float distSq = dx*dx + dz*dz;
        float rSq    = radius * radius;
        if (distSq < rSq && distSq > 0.0f) {
            float dist = std::sqrt(distSq);
            float strength = (radius - dist) / radius;
            force.x += (dx / dist) * strength;
            force.z += (dz / dist) * strength;
        }
#endif
    }
    return force;
}

} // namespace glory
