#pragma once

#include <entt.hpp>
#include <glm/glm.hpp>

namespace glory {

class MinionHealthBars {
public:
  void draw(entt::registry &registry, const glm::mat4 &viewProj,
            float screenW, float screenH, const glm::vec3 &cameraPos,
            float maxRenderDist = 60.0f);
};

} // namespace glory
