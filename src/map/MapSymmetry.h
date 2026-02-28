#pragma once

#include "map/MapTypes.h"

#include <glm/glm.hpp>

#include <vector>

namespace glory {
namespace MapSymmetry {

// Point-reflection across map center (100, y, 100).
// Y is preserved — height doesn't mirror.
inline glm::vec3 MirrorPoint(const glm::vec3 &point,
                             const glm::vec3 &center = {100.f, 0.f, 100.f}) {
  return glm::vec3(2.0f * center.x - point.x, point.y,
                   2.0f * center.z - point.z);
}

// Mirror a full waypoint path.
// Mirroring each point inherently swaps start/end positions (Blue base ↔ Red
// base), so the path direction reverses naturally. No explicit reversal needed.
inline std::vector<glm::vec3> MirrorPath(const std::vector<glm::vec3> &path,
                                         const glm::vec3 &center = {100.f, 0.f,
                                                                    100.f}) {
  std::vector<glm::vec3> mirrored;
  mirrored.reserve(path.size());
  for (const auto &point : path) {
    mirrored.push_back(MirrorPoint(point, center));
  }
  return mirrored;
}

// Mirror a tower, respecting optional team2Override.
inline Tower MirrorTower(const Tower &src,
                         const glm::vec3 &center = {100.f, 0.f, 100.f}) {
  Tower mirrored = src;
  mirrored.position =
      src.team2Override.value_or(MirrorPoint(src.position, center));
  mirrored.team2Override = std::nullopt;
  return mirrored;
}

inline Inhibitor MirrorInhibitor(const Inhibitor &src,
                                 const glm::vec3 &center = {100.f, 0.f,
                                                            100.f}) {
  Inhibitor mirrored = src;
  mirrored.position =
      src.team2Override.value_or(MirrorPoint(src.position, center));
  mirrored.team2Override = std::nullopt;
  return mirrored;
}

inline Base MirrorBase(const Base &src,
                       const glm::vec3 &center = {100.f, 0.f, 100.f}) {
  Base mirrored;
  mirrored.nexusPosition = MirrorPoint(src.nexusPosition, center);
  mirrored.spawnPlatformCenter = MirrorPoint(src.spawnPlatformCenter, center);
  mirrored.spawnPlatformRadius = src.spawnPlatformRadius;
  mirrored.shopPosition = MirrorPoint(src.shopPosition, center);
  return mirrored;
}

inline BrushZone MirrorBrush(const BrushZone &src,
                             const glm::vec3 &center = {100.f, 0.f, 100.f}) {
  BrushZone mirrored = src;
  mirrored.center = src.team2Override.value_or(MirrorPoint(src.center, center));
  mirrored.team2Override = std::nullopt;
  return mirrored;
}

} // namespace MapSymmetry
} // namespace glory
