// ── MapLoader Unit Tests ────────────────────────────────────────────────────
// Standalone executable — uses assert() for validation.
// Requires MAP_DATA_DIR to be defined at compile time pointing to assets/maps/.

#include "map/MapLoader.h"
#include "map/MapTypes.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

using namespace glory;

#ifndef MAP_DATA_DIR
#define MAP_DATA_DIR "assets/maps/"
#endif

static bool vec3Near(const glm::vec3 &a, const glm::vec3 &b,
                     float eps = 0.001f) {
  return glm::length(a - b) < eps;
}

static MapData g_mapData;

// ── Loading tests ───────────────────────────────────────────────────────────

void test_load_json() {
  std::string path = std::string(MAP_DATA_DIR) + "map_summonersrift.json";
  g_mapData = MapLoader::LoadFromFile(path);
  assert(g_mapData.mapName == "Summoner's Rift");
  assert(g_mapData.version == "1.0.0");
  std::printf("  PASS: JSON loads successfully\n");
}

// ── Team 1 tests ────────────────────────────────────────────────────────────

void test_team1_base() {
  const auto &base = g_mapData.teams[0].base;
  assert(vec3Near(base.nexusPosition, {22, 0, 22}));
  assert(vec3Near(base.spawnPlatformCenter, {15, 0, 15}));
  assert(std::abs(base.spawnPlatformRadius - 8.0f) < 0.01f);
  assert(vec3Near(base.shopPosition, {12, 0, 12}));
  std::printf("  PASS: Team 1 base positions correct\n");
}

void test_team1_tower_count() {
  assert(g_mapData.teams[0].towers.size() == 11);
  std::printf("  PASS: Team 1 has 11 towers\n");
}

void test_team1_inhibitor_count() {
  assert(g_mapData.teams[0].inhibitors.size() == 3);
  std::printf("  PASS: Team 1 has 3 inhibitors\n");
}

void test_team1_lane_waypoints() {
  // Mid lane first waypoint should be near Team 1 base
  const auto &midLane = g_mapData.teams[0].lanes[1]; // Mid = index 1
  assert(midLane.waypoints.size() == 9);
  assert(vec3Near(midLane.waypoints.front(), {22, 0, 22}));
  assert(vec3Near(midLane.waypoints.back(), {178, 0, 178}));
  std::printf("  PASS: Team 1 Mid lane has correct waypoint endpoints\n");
}

// ── Team 2 (generated via mirror) tests ─────────────────────────────────────

void test_team2_generated() {
  assert(g_mapData.teams[1].towers.size() == 11);
  assert(g_mapData.teams[1].inhibitors.size() == 3);
  std::printf("  PASS: Team 2 generated with correct entity counts\n");
}

void test_team2_base_mirrored() {
  const auto &base = g_mapData.teams[1].base;
  assert(vec3Near(base.nexusPosition, {178, 0, 178}));
  assert(vec3Near(base.spawnPlatformCenter, {185, 0, 185}));
  assert(vec3Near(base.shopPosition, {188, 0, 188}));
  std::printf("  PASS: Team 2 base correctly mirrored\n");
}

void test_team2_tower_positions() {
  // Blue team outer Top tower is at (26, 0, 102)
  // Mirrored: (200-26, 0, 200-102) = (174, 0, 98)
  const auto &towers = g_mapData.teams[1].towers;
  assert(vec3Near(towers[0].position, {174, 0, 98}));
  std::printf("  PASS: Team 2 tower positions correctly mirrored\n");
}

void test_team2_lane_mirrored() {
  // Team 2 Mid lane: each waypoint is the mirror of Team 1's corresponding
  // waypoint. T1 mid: (22,0,22) -> (178,0,178) T2 mid: mirror each ->
  // (178,0,178) -> (22,0,22) So T2 starts near Red nexus (178,0,178) and ends
  // near Blue nexus (22,0,22).
  const auto &midLane = g_mapData.teams[1].lanes[1];
  assert(midLane.waypoints.size() == 9);
  assert(vec3Near(midLane.waypoints.front(), {178, 0, 178}));
  assert(vec3Near(midLane.waypoints.back(), {22, 0, 22}));
  std::printf("  PASS: Team 2 lane waypoints correctly mirrored\n");
}

// ── Neutral camps tests ─────────────────────────────────────────────────────

void test_neutral_camps() {
  assert(g_mapData.neutralCamps.size() == 11);
  std::printf("  PASS: All 11 neutral camps loaded\n");
}

// ── Brush & Wall tests ──────────────────────────────────────────────────────

void test_brush_zones() {
  assert(g_mapData.brushZones.size() == 8);
  std::printf("  PASS: 8 brush zones loaded\n");
}

void test_walls() {
  assert(g_mapData.walls.size() == 4);
  std::printf("  PASS: 4 wall segments loaded\n");
}

// ── Validation test ─────────────────────────────────────────────────────────

void test_validation_passes() {
  std::string errors;
  bool valid = MapLoader::Validate(g_mapData, errors);
  if (!valid) {
    std::printf("  Validation errors:\n%s\n", errors.c_str());
  }
  assert(valid);
  std::printf("  PASS: MapLoader::Validate passes\n");
}

int main() {
  std::printf("=== MapLoader Tests ===\n");

  test_load_json();
  test_team1_base();
  test_team1_tower_count();
  test_team1_inhibitor_count();
  test_team1_lane_waypoints();
  test_team2_generated();
  test_team2_base_mirrored();
  test_team2_tower_positions();
  test_team2_lane_mirrored();
  test_neutral_camps();
  test_brush_zones();
  test_walls();
  test_validation_passes();

  std::printf("\nAll %d tests passed!\n", 13);
  return 0;
}
