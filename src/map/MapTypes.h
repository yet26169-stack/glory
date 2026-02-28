#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace glory {

// ── Enumerations ────────────────────────────────────────────────────────────

enum class TeamID : uint8_t { Blue = 0, Red = 1, Neutral = 2 };

enum class LaneType : uint8_t { Top = 0, Mid = 1, Bot = 2, Count = 3 };

enum class TowerTier : uint8_t {
  Outer = 0,
  Inner = 1,
  Inhibitor = 2,
  Nexus = 3
};

enum class CampType : uint8_t {
  RedBuff,
  BlueBuff,
  Wolves,
  Raptors,
  Gromp,
  Krugs,
  Scuttler,
  Dragon,
  Baron,
  Herald
};

enum class EntityType : uint8_t {
  Tower,
  Inhibitor,
  Nexus,
  NeutralCamp,
  Spawner
};

// ── Core Position Structures ────────────────────────────────────────────────

struct Base {
  glm::vec3 nexusPosition{0.0f};
  glm::vec3 spawnPlatformCenter{0.0f};
  float spawnPlatformRadius = 8.0f;
  glm::vec3 shopPosition{0.0f};
};

struct Tower {
  glm::vec3 position{0.0f};
  std::optional<glm::vec3> team2Override;
  LaneType lane = LaneType::Mid;
  TowerTier tier = TowerTier::Outer;
  float attackRange = 15.0f;
  float maxHealth = 3500.0f;
  float attackDamage = 150.0f;
};

struct Inhibitor {
  glm::vec3 position{0.0f};
  std::optional<glm::vec3> team2Override;
  LaneType lane = LaneType::Mid;
  float maxHealth = 4000.0f;
  float respawnTime = 300.0f;
};

struct Lane {
  LaneType type = LaneType::Mid;
  std::vector<glm::vec3> waypoints;
  float width = 12.0f;
};

struct NeutralCamp {
  glm::vec3 position{0.0f};
  CampType campType = CampType::Dragon;
  float spawnTime = 90.0f;
  float respawnTime = 300.0f;
  float leashRadius = 8.0f;
  std::vector<glm::vec3> mobPositions;
};

struct BrushZone {
  glm::vec3 center{0.0f};
  glm::vec3 halfExtents{0.0f};
  std::optional<glm::vec3> team2Override;
};

struct WallSegment {
  glm::vec3 start{0.0f};
  glm::vec3 end{0.0f};
  float thickness = 1.0f;
  float height = 3.0f;
};

// ── Aggregate Map Data ──────────────────────────────────────────────────────

struct TeamData {
  Base base;
  std::vector<Tower> towers;
  std::vector<Inhibitor> inhibitors;
  std::array<Lane, 3> lanes; // Top, Mid, Bot
};

struct MapData {
  std::string mapName;
  std::string version;
  glm::vec3 mapCenter = {100.0f, 0.0f, 100.0f};
  glm::vec3 mapBoundsMin = {0.0f, 0.0f, 0.0f};
  glm::vec3 mapBoundsMax = {200.0f, 20.0f, 200.0f};

  TeamData teams[2]; // [0] = Blue, [1] = Red (auto-generated)
  std::vector<NeutralCamp> neutralCamps;
  std::vector<BrushZone> brushZones;
  std::vector<WallSegment> walls;

  const Lane &GetLane(TeamID team, LaneType lane) const {
    return teams[static_cast<int>(team)].lanes[static_cast<int>(lane)];
  }
};

} // namespace glory
