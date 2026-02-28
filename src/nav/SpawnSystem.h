#pragma once

#include "map/MapTypes.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace glory {

enum class SpawnEntityType : uint8_t {
  Tower,
  Inhibitor,
  Nexus,
  NeutralCamp,
  SpawnPlatform
};

struct SpawnCommand {
  SpawnEntityType type;
  TeamID team = TeamID::Neutral;
  glm::vec3 position = glm::vec3(0.0f);
  glm::vec3 rotation = glm::vec3(0.0f); // Euler angles
  float scale = 1.0f;

  // Gameplay
  float maxHealth = 0.0f;
  float attackRange = 0.0f;
  float attackDamage = 0.0f;
  LaneType lane = LaneType::Mid;
  TowerTier tier = TowerTier::Outer;
  CampType campType = CampType::Dragon;

  // Debug
  std::string debugName;
};

class SpawnSystem {
public:
  /// Generate a flat list of all entities to spawn from map data.
  static std::vector<SpawnCommand>
  GenerateSpawnCommands(const MapData &mapData);

private:
  /// Compute tower rotation to face toward the lane center.
  static glm::vec3 ComputeTowerRotation(const Tower &tower,
                                        const MapData &mapData, TeamID team);

  /// Format debug name: "Blue_Top_Outer", "Red_Nexus", etc.
  static std::string FormatName(TeamID team, const std::string &suffix);
};

// ── Inline implementation ───────────────────────────────────────────────────

inline std::string SpawnSystem::FormatName(TeamID team,
                                           const std::string &suffix) {
  return (team == TeamID::Blue ? "Blue_" : "Red_") + suffix;
}

inline glm::vec3 SpawnSystem::ComputeTowerRotation(const Tower &tower,
                                                   const MapData &mapData,
                                                   TeamID team) {
  // Find the lane this tower belongs to
  int teamIdx = (team == TeamID::Blue) ? 0 : 1;
  for (const auto &lane : mapData.teams[teamIdx].lanes) {
    if (lane.type == tower.lane && lane.waypoints.size() >= 2) {
      // Tower should face toward the lane midpoint
      glm::vec3 mid = lane.waypoints[lane.waypoints.size() / 2];
      glm::vec3 dir = mid - tower.position;
      float yaw = std::atan2(dir.x, dir.z);
      return glm::vec3(0.0f, yaw, 0.0f);
    }
  }
  return glm::vec3(0.0f);
}

inline std::vector<SpawnCommand>
SpawnSystem::GenerateSpawnCommands(const MapData &mapData) {
  std::vector<SpawnCommand> commands;
  commands.reserve(64);

  auto laneStr = [](LaneType l) -> std::string {
    switch (l) {
    case LaneType::Top:
      return "Top";
    case LaneType::Mid:
      return "Mid";
    case LaneType::Bot:
      return "Bot";
    default:
      return "Unknown";
    }
  };

  auto tierStr = [](TowerTier t) -> std::string {
    switch (t) {
    case TowerTier::Outer:
      return "Outer";
    case TowerTier::Inner:
      return "Inner";
    case TowerTier::Inhibitor:
      return "Inhib";
    case TowerTier::Nexus:
      return "Nexus";
    }
    return "Unknown";
  };

  auto campStr = [](CampType c) -> std::string {
    switch (c) {
    case CampType::Wolves:
      return "Wolves";
    case CampType::Raptors:
      return "Raptors";
    case CampType::Gromp:
      return "Gromp";
    case CampType::Krugs:
      return "Krugs";
    case CampType::Scuttler:
      return "Scuttler";
    case CampType::Dragon:
      return "Dragon";
    case CampType::Baron:
      return "Baron";
    case CampType::Herald:
      return "Herald";
    case CampType::RedBuff:
      return "RedBuff";
    case CampType::BlueBuff:
      return "BlueBuff";
    }
    return "Camp";
  };

  // For each team
  for (int ti = 0; ti < 2; ++ti) {
    TeamID team = (ti == 0) ? TeamID::Blue : TeamID::Red;
    const auto &teamData = mapData.teams[ti];
    const auto &base = teamData.base;

    // Nexus
    {
      SpawnCommand cmd{};
      cmd.type = SpawnEntityType::Nexus;
      cmd.team = team;
      cmd.position = base.nexusPosition;
      cmd.maxHealth = 5500.0f; // Standard nexus health
      cmd.debugName = FormatName(team, "Nexus");
      commands.push_back(cmd);
    }

    // Spawn Platform
    {
      SpawnCommand cmd{};
      cmd.type = SpawnEntityType::SpawnPlatform;
      cmd.team = team;
      cmd.position = base.spawnPlatformCenter;
      cmd.scale = base.spawnPlatformRadius;
      cmd.debugName = FormatName(team, "SpawnPlatform");
      commands.push_back(cmd);
    }

    // Towers
    for (const auto &tower : teamData.towers) {
      SpawnCommand cmd{};
      cmd.type = SpawnEntityType::Tower;
      cmd.team = team;
      cmd.position = tower.position;
      cmd.rotation = ComputeTowerRotation(tower, mapData, team);
      cmd.maxHealth = tower.maxHealth;
      cmd.attackRange = tower.attackRange;
      cmd.attackDamage = tower.attackDamage;
      cmd.lane = tower.lane;
      cmd.tier = tower.tier;
      cmd.debugName =
          FormatName(team, laneStr(tower.lane) + "_" + tierStr(tower.tier));
      commands.push_back(cmd);
    }

    // Inhibitors
    for (const auto &inhib : teamData.inhibitors) {
      SpawnCommand cmd{};
      cmd.type = SpawnEntityType::Inhibitor;
      cmd.team = team;
      cmd.position = inhib.position;
      cmd.maxHealth = inhib.maxHealth;
      cmd.lane = inhib.lane;
      cmd.debugName = FormatName(team, laneStr(inhib.lane) + "_Inhibitor");
      commands.push_back(cmd);
    }
  }

  // Neutral Camps
  for (const auto &camp : mapData.neutralCamps) {
    SpawnCommand cmd{};
    cmd.type = SpawnEntityType::NeutralCamp;
    cmd.team = TeamID::Neutral;
    cmd.position = camp.position;
    cmd.campType = camp.campType;
    cmd.debugName = campStr(camp.campType);
    commands.push_back(cmd);
  }

  return commands;
}

} // namespace glory
