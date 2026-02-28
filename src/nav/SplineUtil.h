#pragma once

#include <glm/glm.hpp>
#include <vector>

namespace glory {

namespace SplineUtil {

/// Catmull-Rom interpolation between p1 and p2.
/// t ranges from 0.0 (at p1) to 1.0 (at p2).
/// p0 and p3 are tangent guide points.
inline glm::vec3 CatmullRom(const glm::vec3 &p0, const glm::vec3 &p1,
                            const glm::vec3 &p2, const glm::vec3 &p3, float t) {
  float t2 = t * t;
  float t3 = t2 * t;
  return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                 (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                 (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

/// Returns a point on the full lane path.
/// globalT ranges from 0.0 (lane start) to 1.0 (lane end).
inline glm::vec3 GetPointOnLane(const std::vector<glm::vec3> &waypoints,
                                float globalT) {
  if (waypoints.empty())
    return glm::vec3(0.0f);
  if (waypoints.size() == 1)
    return waypoints[0];

  int N = static_cast<int>(waypoints.size());
  int numSegments = N - 1;
  float scaledT = std::clamp(globalT, 0.0f, 1.0f) * numSegments;
  int segment = static_cast<int>(std::floor(scaledT));
  segment = std::clamp(segment, 0, numSegments - 1);
  float localT = scaledT - static_cast<float>(segment);

  const glm::vec3 &p0 = waypoints[std::max(0, segment - 1)];
  const glm::vec3 &p1 = waypoints[segment];
  const glm::vec3 &p2 = waypoints[std::min(N - 1, segment + 1)];
  const glm::vec3 &p3 = waypoints[std::min(N - 1, segment + 2)];

  return CatmullRom(p0, p1, p2, p3, localT);
}

/// Returns the tangent direction at a globalT position on the lane.
inline glm::vec3 GetTangentOnLane(const std::vector<glm::vec3> &waypoints,
                                  float globalT) {
  float epsilon = 0.001f;
  float t0 = std::max(0.0f, globalT - epsilon);
  float t1 = std::min(1.0f, globalT + epsilon);
  glm::vec3 p0 = GetPointOnLane(waypoints, t0);
  glm::vec3 p1 = GetPointOnLane(waypoints, t1);
  glm::vec3 dir = p1 - p0;
  float len = glm::length(dir);
  return len > 0.0001f ? dir / len : glm::vec3(1, 0, 0);
}

/// Compute total arc length of a lane path by numerical integration.
/// steps = number of subdivisions (higher = more accurate).
inline float TotalArcLength(const std::vector<glm::vec3> &waypoints,
                            int steps = 256) {
  if (waypoints.size() < 2)
    return 0.0f;
  float length = 0.0f;
  glm::vec3 prev = GetPointOnLane(waypoints, 0.0f);
  for (int i = 1; i <= steps; ++i) {
    float t = static_cast<float>(i) / steps;
    glm::vec3 curr = GetPointOnLane(waypoints, t);
    length += glm::length(curr - prev);
    prev = curr;
  }
  return length;
}

/// Convert arc-length distance to globalT using binary search.
inline float ArcLengthToT(const std::vector<glm::vec3> &waypoints,
                          float distance, int steps = 256) {
  float totalLen = TotalArcLength(waypoints, steps);
  if (totalLen < 0.0001f)
    return 0.0f;

  float targetFraction = std::clamp(distance / totalLen, 0.0f, 1.0f);

  // Binary search for the t value
  float lo = 0.0f, hi = 1.0f;
  for (int iter = 0; iter < 20; ++iter) {
    float mid = (lo + hi) * 0.5f;
    // Compute arc length from 0 to mid
    float len = 0.0f;
    glm::vec3 prev = GetPointOnLane(waypoints, 0.0f);
    int subSteps = steps / 4;
    for (int i = 1; i <= subSteps; ++i) {
      float t = mid * static_cast<float>(i) / subSteps;
      glm::vec3 curr = GetPointOnLane(waypoints, t);
      len += glm::length(curr - prev);
      prev = curr;
    }
    float fraction = len / totalLen;
    if (fraction < targetFraction)
      lo = mid;
    else
      hi = mid;
  }
  return (lo + hi) * 0.5f;
}

} // namespace SplineUtil
} // namespace glory
