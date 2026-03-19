// ── test_fog.cpp ────────────────────────────────────────────────────────────
// CPU-only tests for FogSystem vision painting, exploration, and visibility.
// No Vulkan required — uses updateCpuOnly().

#include "fog/FogSystem.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace glory;

// Helper: count non-zero pixels in buffer
static int countNonZero(const std::vector<uint8_t> &buf) {
  int count = 0;
  for (auto v : buf)
    if (v > 0)
      ++count;
  return count;
}

// ── Tests ───────────────────────────────────────────────────────────────────

void test_vision_circle_paints_pixels() {
  FogSystem fog;
  // Manually init CPU buffers (no Vulkan)
  auto &vBuf = const_cast<std::vector<uint8_t> &>(fog.getVisionBuffer());
  auto &eBuf = const_cast<std::vector<uint8_t> &>(fog.getExplorationBuffer());
  vBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);
  eBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);

  VisionEntity entity;
  entity.position = glm::vec3(100.0f, 0.0f, 100.0f); // map center
  entity.sightRange = 20.0f;

  fog.updateCpuOnly({entity});

  int painted = countNonZero(fog.getVisionBuffer());
  assert(painted > 0);

  // Center pixel should be fully visible (255)
  int cx =
      static_cast<int>((100.0f / FogSystem::WORLD_SIZE) * FogSystem::MAP_SIZE);
  int cz = cx;
  uint8_t centerVal = fog.getVisionBuffer()[cz * FogSystem::MAP_SIZE + cx];
  assert(centerVal == 255);

  std::printf("  PASS: Vision circle paints %d pixels, center=%d\n", painted,
              centerVal);
}

void test_vision_clears_each_frame() {
  FogSystem fog;
  auto &vBuf = const_cast<std::vector<uint8_t> &>(fog.getVisionBuffer());
  auto &eBuf = const_cast<std::vector<uint8_t> &>(fog.getExplorationBuffer());
  vBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);
  eBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);

  VisionEntity entity;
  entity.position = glm::vec3(50.0f, 0.0f, 50.0f);
  entity.sightRange = 15.0f;
  fog.updateCpuOnly({entity});
  int firstPaint = countNonZero(fog.getVisionBuffer());
  assert(firstPaint > 0);

  // Update with no entities — vision should be cleared
  fog.updateCpuOnly({});
  int afterClear = countNonZero(fog.getVisionBuffer());
  assert(afterClear == 0);

  std::printf("  PASS: Vision clears each frame (%d → %d pixels)\n", firstPaint,
              afterClear);
}

void test_exploration_persists() {
  FogSystem fog;
  auto &vBuf = const_cast<std::vector<uint8_t> &>(fog.getVisionBuffer());
  auto &eBuf = const_cast<std::vector<uint8_t> &>(fog.getExplorationBuffer());
  vBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);
  eBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);

  VisionEntity entity;
  entity.position = glm::vec3(100.0f, 0.0f, 100.0f);
  entity.sightRange = 20.0f;
  fog.updateCpuOnly({entity});
  int exploredAfterFirst = countNonZero(fog.getExplorationBuffer());

  // Remove entity — exploration should persist
  fog.updateCpuOnly({});
  int exploredAfterClear = countNonZero(fog.getExplorationBuffer());
  assert(exploredAfterClear == exploredAfterFirst);
  assert(exploredAfterClear > 0);

  std::printf("  PASS: Exploration persists (%d pixels remain)\n",
              exploredAfterClear);
}

void test_visibility_query() {
  FogSystem fog;
  auto &vBuf = const_cast<std::vector<uint8_t> &>(fog.getVisionBuffer());
  auto &eBuf = const_cast<std::vector<uint8_t> &>(fog.getExplorationBuffer());
  vBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);
  eBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);

  VisionEntity entity;
  entity.position = glm::vec3(100.0f, 0.0f, 100.0f);
  entity.sightRange = 20.0f;
  fog.updateCpuOnly({entity});

  // Center should be visible
  assert(fog.isPositionVisible(100.0f, 100.0f));
  // Far corner should NOT be visible
  assert(!fog.isPositionVisible(0.0f, 0.0f));

  std::printf(
      "  PASS: Visibility query correct (center=visible, corner=hidden)\n");
}

void test_exploration_query() {
  FogSystem fog;
  auto &vBuf = const_cast<std::vector<uint8_t> &>(fog.getVisionBuffer());
  auto &eBuf = const_cast<std::vector<uint8_t> &>(fog.getExplorationBuffer());
  vBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);
  eBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);

  VisionEntity entity;
  entity.position = glm::vec3(100.0f, 0.0f, 100.0f);
  entity.sightRange = 20.0f;
  fog.updateCpuOnly({entity});

  // Clear vision
  fog.updateCpuOnly({});
  assert(!fog.isPositionVisible(100.0f, 100.0f)); // no longer visible
  assert(fog.isPositionExplored(100.0f, 100.0f)); // but explored
  assert(!fog.isPositionExplored(0.0f, 0.0f));    // never explored

  std::printf("  PASS: Exploration query correct\n");
}

void test_soft_falloff() {
  FogSystem fog;
  auto &vBuf = const_cast<std::vector<uint8_t> &>(fog.getVisionBuffer());
  auto &eBuf = const_cast<std::vector<uint8_t> &>(fog.getExplorationBuffer());
  vBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);
  eBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);

  VisionEntity entity;
  entity.position = glm::vec3(100.0f, 0.0f, 100.0f);
  entity.sightRange = 30.0f;
  fog.updateCpuOnly({entity});

  // Center pixel should be 255
  int cx =
      static_cast<int>((100.0f / FogSystem::WORLD_SIZE) * FogSystem::MAP_SIZE);
  uint8_t center = fog.getVisionBuffer()[cx * FogSystem::MAP_SIZE + cx];
  assert(center == 255);

  // Edge pixel should be < 255 (soft falloff)
  float edgeDist = entity.sightRange * 0.95f;
  float edgeX = 100.0f + edgeDist;
  int ex =
      static_cast<int>((edgeX / FogSystem::WORLD_SIZE) * FogSystem::MAP_SIZE);
  ex = std::clamp(ex, 0, static_cast<int>(FogSystem::MAP_SIZE) - 1);
  uint8_t edgeVal = fog.getVisionBuffer()[cx * FogSystem::MAP_SIZE + ex];
  assert(edgeVal < 255);

  std::printf("  PASS: Soft falloff (center=%d, edge=%d)\n", center, edgeVal);
}

void test_multiple_entities_merge() {
  FogSystem fog;
  auto &vBuf = const_cast<std::vector<uint8_t> &>(fog.getVisionBuffer());
  auto &eBuf = const_cast<std::vector<uint8_t> &>(fog.getExplorationBuffer());
  vBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);
  eBuf.resize(FogSystem::MAP_SIZE * FogSystem::MAP_SIZE, 0);

  std::vector<VisionEntity> entities = {{{50.0f, 0.0f, 100.0f}, 15.0f},
                                        {{150.0f, 0.0f, 100.0f}, 15.0f}};
  fog.updateCpuOnly(entities);

  assert(fog.isPositionVisible(50.0f, 100.0f));
  assert(fog.isPositionVisible(150.0f, 100.0f));
  assert(!fog.isPositionVisible(100.0f, 100.0f)); // gap between

  std::printf("  PASS: Multiple entities create separate vision regions\n");
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
  std::printf("=== Fog of War Tests ===\n");

  test_vision_circle_paints_pixels();
  test_vision_clears_each_frame();
  test_exploration_persists();
  test_visibility_query();
  test_exploration_query();
  test_soft_falloff();
  test_multiple_entities_merge();

  std::printf("\nAll 7 tests passed!\n");
  return 0;
}
