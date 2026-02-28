#pragma once

#include "map/MapTypes.h"
#include "nav/SplineUtil.h"

#include <glm/glm.hpp>
#include <vector>

namespace glory {

/// Component for an entity that follows a lane path.
struct LaneFollower {
  TeamID team = TeamID::Blue;
  LaneType lane = LaneType::Mid;
  float progress = 0.0f;  // 0 = own base, 1 = enemy base
  float moveSpeed = 6.0f; // units per second
  glm::vec3 currentPos = glm::vec3(0.0f);
  glm::vec3 targetPos = glm::vec3(0.0f); // deviation target
  bool isDeviating = false;
  float returnTimer = 0.0f;
  glm::vec3 laneReturnPos = glm::vec3(0.0f);

  /// Advance along the lane. Returns new world position.
  /// heightQuery: function returning terrain height at (x, z).
  void update(float dt, const std::vector<glm::vec3> &waypoints,
              float totalArcLength,
              float (*heightQuery)(float, float) = nullptr) {
    if (isDeviating) {
      // Move toward target
      glm::vec3 dir = targetPos - currentPos;
      float dist = glm::length(dir);
      if (dist > 0.1f) {
        currentPos += (dir / dist) * moveSpeed * dt;
      } else {
        isDeviating = false;
      }
      returnTimer -= dt;
      if (returnTimer <= 0.0f) {
        isDeviating = false;
      }
      return;
    }

    if (totalArcLength > 0.0001f) {
      progress += (moveSpeed * dt) / totalArcLength;
    }
    progress = std::clamp(progress, 0.0f, 1.0f);
    currentPos = SplineUtil::GetPointOnLane(waypoints, progress);

    // Snap Y to terrain height
    if (heightQuery) {
      currentPos.y = heightQuery(currentPos.x, currentPos.z);
    }
  }

  /// Start deviating toward a target (e.g., aggro).
  void deviate(const glm::vec3 &target, float maxTime = 3.0f) {
    isDeviating = true;
    targetPos = target;
    returnTimer = maxTime;
    laneReturnPos = currentPos;
  }
};

} // namespace glory
