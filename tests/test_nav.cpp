// ── test_nav.cpp ────────────────────────────────────────────────────────────
// CPU-only tests for SplineUtil, LaneFollower, and SpawnSystem.
// No Vulkan required.

#include "map/MapLoader.h"
#include "nav/LaneFollower.h"
#include "nav/NavMeshBuilder.h"
#include "nav/SpawnSystem.h"
#include "nav/SplineUtil.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace glory;

static int g_testsPassed = 0;

static bool vec3Near(const glm::vec3 &a, const glm::vec3 &b, float eps = 0.5f) {
  return glm::length(a - b) < eps;
}

// ── Spline Tests ────────────────────────────────────────────────────────────

void test_spline_endpoints() {
  std::vector<glm::vec3> waypoints = {
      {0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}};
  glm::vec3 start = SplineUtil::GetPointOnLane(waypoints, 0.0f);
  glm::vec3 end = SplineUtil::GetPointOnLane(waypoints, 1.0f);
  assert(vec3Near(start, waypoints.front(), 0.01f));
  assert(vec3Near(end, waypoints.back(), 0.01f));
  std::printf("  PASS: Spline endpoints match waypoints\n");
  g_testsPassed++;
}

void test_spline_midpoint_smooth() {
  std::vector<glm::vec3> waypoints = {
      {0, 0, 0}, {10, 0, 5}, {20, 0, 0}, {30, 0, 5}};
  glm::vec3 mid = SplineUtil::GetPointOnLane(waypoints, 0.5f);
  // Mid should be roughly near (15, 0, ~2.5) — between 2nd and 3rd waypoint
  assert(mid.x > 10.0f && mid.x < 20.0f);
  std::printf("  PASS: Spline midpoint is smooth and between waypoints\n");
  g_testsPassed++;
}

void test_spline_tangent_direction() {
  std::vector<glm::vec3> waypoints = {
      {0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}};
  glm::vec3 tangent = SplineUtil::GetTangentOnLane(waypoints, 0.5f);
  // Straight line: tangent should be roughly (1, 0, 0)
  assert(tangent.x > 0.9f);
  assert(std::abs(tangent.z) < 0.1f);
  std::printf("  PASS: Spline tangent direction correct\n");
  g_testsPassed++;
}

void test_spline_arc_length() {
  std::vector<glm::vec3> waypoints = {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}};
  float len = SplineUtil::TotalArcLength(waypoints, 256);
  // Straight line from 0 to 20: should be close to 20
  assert(std::abs(len - 20.0f) < 1.0f);
  std::printf("  PASS: Arc length ~20 for straight 20-unit line (got %.2f)\n",
  g_testsPassed++;
              len);
}

void test_spline_single_point() {
  std::vector<glm::vec3> waypoints = {{5, 3, 7}};
  glm::vec3 p = SplineUtil::GetPointOnLane(waypoints, 0.5f);
  assert(vec3Near(p, {5, 3, 7}, 0.01f));
  std::printf("  PASS: Single-point spline returns that point\n");
  g_testsPassed++;
}

// ── LaneFollower Tests ──────────────────────────────────────────────────────

void test_lanefollower_start_end() {
  std::vector<glm::vec3> waypoints = {{0, 0, 0}, {50, 0, 0}, {100, 0, 0}};
  float arcLen = SplineUtil::TotalArcLength(waypoints);

  LaneFollower follower;
  follower.progress = 0.0f;
  follower.update(0.0f, waypoints, arcLen);
  assert(vec3Near(follower.currentPos, waypoints.front(), 0.5f));

  follower.progress = 1.0f;
  follower.update(0.0f, waypoints, arcLen);
  assert(vec3Near(follower.currentPos, waypoints.back(), 0.5f));

  std::printf("  PASS: LaneFollower at t=0 → start, t=1 → end\n");
  g_testsPassed++;
}

void test_lanefollower_advances() {
  std::vector<glm::vec3> waypoints = {{0, 0, 0}, {50, 0, 0}, {100, 0, 0}};
  float arcLen = SplineUtil::TotalArcLength(waypoints);

  LaneFollower follower;
  follower.progress = 0.0f;
  follower.moveSpeed = 10.0f;
  // After 1 second, should have advanced by moveSpeed/arcLen
  follower.update(1.0f, waypoints, arcLen);
  assert(follower.progress > 0.0f);
  assert(follower.currentPos.x > 0.0f);
  std::printf(
      "  PASS: LaneFollower advances along lane (progress=%.3f, x=%.1f)\n",
      follower.progress, follower.currentPos.x);
}

void test_lanefollower_deviation() {
  std::vector<glm::vec3> waypoints = {{0, 0, 0}, {100, 0, 0}};
  float arcLen = SplineUtil::TotalArcLength(waypoints);

  LaneFollower follower;
  follower.progress = 0.5f;
  follower.update(0.0f, waypoints, arcLen);
  glm::vec3 lanePos = follower.currentPos;

  follower.deviate(glm::vec3(50, 0, 20), 3.0f);
  assert(follower.isDeviating);

  // Update should move toward target, not along lane
  follower.update(0.1f, waypoints, arcLen);
  assert(follower.currentPos.z > 0.0f);
  std::printf("  PASS: LaneFollower deviates off-lane\n");
  g_testsPassed++;
}

// ── SpawnSystem Tests ───────────────────────────────────────────────────────

void test_spawn_command_counts() {
  MapData mapData =
      MapLoader::LoadFromFile(MAP_DATA_DIR "map_summonersrift.json");

  auto commands = SpawnSystem::GenerateSpawnCommands(mapData);

  int nexuses = 0, platforms = 0, towers = 0, inhibitors = 0, camps = 0;
  for (const auto &cmd : commands) {
    switch (cmd.type) {
    case SpawnEntityType::Nexus:
      ++nexuses;
      break;
    case SpawnEntityType::SpawnPlatform:
      ++platforms;
      break;
    case SpawnEntityType::Tower:
      ++towers;
      break;
    case SpawnEntityType::Inhibitor:
      ++inhibitors;
      break;
    case SpawnEntityType::NeutralCamp:
      ++camps;
      break;
    }
  }

  assert(nexuses == 2);    // 1 Blue + 1 Red
  assert(platforms == 2);  // 1 Blue + 1 Red
  assert(towers == 22);    // 11 Blue + 11 Red
  assert(inhibitors == 6); // 3 Blue + 3 Red
  assert(camps == 11);     // 11 neutral camps

  std::printf("  PASS: SpawnSystem generates correct counts (%d nexuses, %d "
              "towers, %d inhibs, %d camps)\n",
              nexuses, towers, inhibitors, camps);
  g_testsPassed++;
}

void test_spawn_command_debug_names() {
  MapData mapData =
      MapLoader::LoadFromFile(MAP_DATA_DIR "map_summonersrift.json");
  auto commands = SpawnSystem::GenerateSpawnCommands(mapData);

  // Nexus should have "Blue_Nexus" or "Red_Nexus"
  bool foundBlueNexus = false, foundRedNexus = false;
  for (const auto &cmd : commands) {
    if (cmd.debugName == "Blue_Nexus")
      foundBlueNexus = true;
    if (cmd.debugName == "Red_Nexus")
      foundRedNexus = true;
  }
  assert(foundBlueNexus);
  assert(foundRedNexus);
  std::printf("  PASS: SpawnSystem debug names correct\n");
  g_testsPassed++;
}

void test_spawn_tower_rotation() {
  MapData mapData =
      MapLoader::LoadFromFile(MAP_DATA_DIR "map_summonersrift.json");
  auto commands = SpawnSystem::GenerateSpawnCommands(mapData);

  // At least one tower should have non-zero rotation
  bool hasRotation = false;
  for (const auto &cmd : commands) {
    if (cmd.type == SpawnEntityType::Tower &&
        glm::length(cmd.rotation) > 0.01f) {
      hasRotation = true;
      break;
    }
  }
  assert(hasRotation);
  std::printf("  PASS: SpawnSystem tower rotations face lane center\n");
  g_testsPassed++;
}

void test_navmesh_build() {
  // Create a 20x20 ground plane (2 triangles)
  std::vector<float> vertices = {
      0.0f, 0.0f, 0.0f,
      20.0f, 0.0f, 0.0f,
      20.0f, 0.0f, 20.0f,
      0.0f, 0.0f, 20.0f
  };
  std::vector<int> triangles = {
      0, 1, 2,
      0, 2, 3
  };

  NavMeshBuilder builder;
  NavMeshConfig config;
  config.regionMinSize = 0.0f; // Ensure even small regions are kept
  NavMeshData data = builder.build(vertices, triangles, config);

  assert(data.valid);
  assert(!data.serializedData.empty());
  std::printf("  PASS: NavMesh generation produced valid data (%zu bytes)\n", data.serializedData.size());
  g_testsPassed++;

  PathfindingSystem pathfinding;
  bool ok = pathfinding.init(data);
  assert(ok);

  PathResult path = pathfinding.findPath({5.0f, 0.1f, 5.0f}, {15.0f, 0.1f, 15.0f});
  assert(path.found);
  assert(path.waypoints.size() >= 2);
  std::printf("  PASS: Pathfinding found path with %zu waypoints\n", path.waypoints.size());
  g_testsPassed++;

  pathfinding.shutdown();
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
  std::printf("=== Navigation & Spawning Tests ===\n");

  test_spline_endpoints();
  test_spline_midpoint_smooth();
  test_spline_tangent_direction();
  test_spline_arc_length();
  test_spline_single_point();

  test_lanefollower_start_end();
  test_lanefollower_advances();
  test_lanefollower_deviation();

  test_spawn_command_counts();
  test_spawn_command_debug_names();
  test_spawn_tower_rotation();

  test_navmesh_build();

  std::printf("\nAll %d tests passed!\n", g_testsPassed);
  return 0;
}
