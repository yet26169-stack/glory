// ── Mirror Symmetry Unit Tests ──────────────────────────────────────────────
// Standalone executable — uses assert() for validation.

#include "map/MapSymmetry.h"
#include "map/MapTypes.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace glory;

static bool vec3Near(const glm::vec3 &a, const glm::vec3 &b,
                     float eps = 0.001f) {
  return glm::length(a - b) < eps;
}

// ── MirrorPoint tests ───────────────────────────────────────────────────────

void test_mirror_point_basic() {
  glm::vec3 result = MapSymmetry::MirrorPoint({30.0f, 0.0f, 40.0f});
  assert(vec3Near(result, {170.0f, 0.0f, 160.0f}));
  std::printf("  PASS: MirrorPoint (30,0,40) -> (170,0,160)\n");
}

void test_mirror_point_roundtrip() {
  glm::vec3 original = {30.0f, 0.0f, 40.0f};
  glm::vec3 mirrored = MapSymmetry::MirrorPoint(original);
  glm::vec3 restored = MapSymmetry::MirrorPoint(mirrored);
  assert(vec3Near(original, restored));
  std::printf("  PASS: MirrorPoint roundtrip\n");
}

void test_mirror_point_center() {
  glm::vec3 result = MapSymmetry::MirrorPoint({100.0f, 5.0f, 100.0f});
  assert(vec3Near(result, {100.0f, 5.0f, 100.0f}));
  std::printf("  PASS: MirrorPoint center remains at center\n");
}

void test_mirror_point_preserves_y() {
  glm::vec3 result = MapSymmetry::MirrorPoint({50.0f, 7.5f, 50.0f});
  assert(std::abs(result.y - 7.5f) < 0.001f);
  std::printf("  PASS: MirrorPoint preserves Y\n");
}

// ── MirrorPath tests ────────────────────────────────────────────────────────

void test_mirror_path_mirrors_positions() {
  std::vector<glm::vec3> path = {
      {22, 0, 22}, {60, 0, 22}, {100, 0, 22}, {140, 0, 22}, {178, 0, 22}};

  auto mirrored = MapSymmetry::MirrorPath(path);

  assert(mirrored.size() == path.size());

  // Each point is mirrored in-place (no reversal)
  assert(vec3Near(mirrored[0], MapSymmetry::MirrorPoint(path[0])));
  assert(vec3Near(mirrored[4], MapSymmetry::MirrorPoint(path[4])));

  // First point: mirror(22,0,22) = (178,0,178), so Red team's path starts near
  // Red base
  assert(vec3Near(mirrored[0], {178, 0, 178}));

  std::printf("  PASS: MirrorPath mirrors positions correctly\n");
}

void test_mirror_path_roundtrip() {
  std::vector<glm::vec3> path = {
      {22, 0, 22}, {40, 0, 40}, {60, 0, 60}, {80, 0, 80}};

  auto mirrored = MapSymmetry::MirrorPath(path);
  auto restored = MapSymmetry::MirrorPath(mirrored);

  assert(restored.size() == path.size());
  for (size_t i = 0; i < path.size(); ++i) {
    assert(vec3Near(path[i], restored[i]));
  }

  std::printf("  PASS: MirrorPath roundtrip\n");
}

// ── MirrorTower tests ───────────────────────────────────────────────────────

void test_mirror_tower_without_override() {
  Tower t;
  t.position = {26.0f, 0.0f, 102.0f};
  t.lane = LaneType::Top;
  t.tier = TowerTier::Outer;

  Tower mirrored = MapSymmetry::MirrorTower(t);
  assert(vec3Near(mirrored.position, {174.0f, 0.0f, 98.0f}));
  assert(!mirrored.team2Override.has_value());
  std::printf("  PASS: MirrorTower without override\n");
}

void test_mirror_tower_with_override() {
  Tower t;
  t.position = {26.0f, 0.0f, 102.0f};
  t.team2Override = glm::vec3{180.0f, 0.0f, 95.0f};
  t.lane = LaneType::Top;
  t.tier = TowerTier::Outer;

  Tower mirrored = MapSymmetry::MirrorTower(t);
  assert(vec3Near(mirrored.position, {180.0f, 0.0f, 95.0f}));
  assert(!mirrored.team2Override.has_value());
  std::printf("  PASS: MirrorTower with override uses override position\n");
}

// ── MirrorBase tests ────────────────────────────────────────────────────────

void test_mirror_base() {
  Base b;
  b.nexusPosition = {22, 0, 22};
  b.spawnPlatformCenter = {15, 0, 15};
  b.spawnPlatformRadius = 8.0f;
  b.shopPosition = {12, 0, 12};

  Base mirrored = MapSymmetry::MirrorBase(b);
  assert(vec3Near(mirrored.nexusPosition, {178, 0, 178}));
  assert(vec3Near(mirrored.spawnPlatformCenter, {185, 0, 185}));
  assert(std::abs(mirrored.spawnPlatformRadius - 8.0f) < 0.001f);
  assert(vec3Near(mirrored.shopPosition, {188, 0, 188}));
  std::printf("  PASS: MirrorBase\n");
}

// ── MirrorInhibitor tests ───────────────────────────────────────────────────

void test_mirror_inhibitor() {
  Inhibitor inh;
  inh.position = {22.0f, 0.0f, 170.0f};
  inh.lane = LaneType::Top;

  Inhibitor mirrored = MapSymmetry::MirrorInhibitor(inh);
  assert(vec3Near(mirrored.position, {178.0f, 0.0f, 30.0f}));
  std::printf("  PASS: MirrorInhibitor\n");
}

int main() {
  std::printf("=== Mirror Symmetry Tests ===\n");

  test_mirror_point_basic();
  test_mirror_point_roundtrip();
  test_mirror_point_center();
  test_mirror_point_preserves_y();
  test_mirror_path_mirrors_positions();
  test_mirror_path_roundtrip();
  test_mirror_tower_without_override();
  test_mirror_tower_with_override();
  test_mirror_base();
  test_mirror_inhibitor();

  std::printf("\nAll %d tests passed!\n", 10);
  return 0;
}
