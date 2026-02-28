#include "map/MapLoader.h"
#include "map/MapSymmetry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

using json = nlohmann::json;

namespace glory {

// ── Parse helpers ───────────────────────────────────────────────────────────

static glm::vec3 ParseVec3(const json &j) {
  return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

static LaneType ParseLaneType(const std::string &str) {
  if (str == "Top")
    return LaneType::Top;
  if (str == "Mid")
    return LaneType::Mid;
  if (str == "Bot")
    return LaneType::Bot;
  throw std::runtime_error("Unknown lane type: " + str);
}

static TowerTier ParseTowerTier(const std::string &str) {
  if (str == "Outer")
    return TowerTier::Outer;
  if (str == "Inner")
    return TowerTier::Inner;
  if (str == "Inhibitor")
    return TowerTier::Inhibitor;
  if (str == "Nexus")
    return TowerTier::Nexus;
  throw std::runtime_error("Unknown tower tier: " + str);
}

static CampType ParseCampType(const std::string &str) {
  if (str == "RedBuff")
    return CampType::RedBuff;
  if (str == "BlueBuff")
    return CampType::BlueBuff;
  if (str == "Wolves")
    return CampType::Wolves;
  if (str == "Raptors")
    return CampType::Raptors;
  if (str == "Gromp")
    return CampType::Gromp;
  if (str == "Krugs")
    return CampType::Krugs;
  if (str == "Scuttler")
    return CampType::Scuttler;
  if (str == "Dragon")
    return CampType::Dragon;
  if (str == "Baron")
    return CampType::Baron;
  if (str == "Herald")
    return CampType::Herald;
  throw std::runtime_error("Unknown camp type: " + str);
}

// ── Team Parsing ────────────────────────────────────────────────────────────

static TeamData ParseTeam1(const json &j) {
  TeamData team;

  // Base
  const auto &base = j["base"];
  team.base.nexusPosition = ParseVec3(base["nexusPosition"]);
  team.base.spawnPlatformCenter = ParseVec3(base["spawnPlatformCenter"]);
  team.base.spawnPlatformRadius = base.value("spawnPlatformRadius", 8.0f);
  team.base.shopPosition = ParseVec3(base["shopPosition"]);

  // Towers
  for (const auto &t : j["towers"]) {
    Tower tower;
    tower.position = ParseVec3(t["position"]);
    tower.lane = ParseLaneType(t["lane"].get<std::string>());
    tower.tier = ParseTowerTier(t["tier"].get<std::string>());
    tower.attackRange = t.value("attackRange", 15.0f);
    tower.maxHealth = t.value("maxHealth", 3500.0f);
    tower.attackDamage = t.value("attackDamage", 150.0f);

    if (t.contains("team2Override")) {
      tower.team2Override = ParseVec3(t["team2Override"]);
    }

    team.towers.push_back(tower);
  }

  // Inhibitors
  for (const auto &inh : j["inhibitors"]) {
    Inhibitor inhib;
    inhib.position = ParseVec3(inh["position"]);
    inhib.lane = ParseLaneType(inh["lane"].get<std::string>());
    inhib.maxHealth = inh.value("maxHealth", 4000.0f);
    inhib.respawnTime = inh.value("respawnTime", 300.0f);

    if (inh.contains("team2Override")) {
      inhib.team2Override = ParseVec3(inh["team2Override"]);
    }

    team.inhibitors.push_back(inhib);
  }

  // Lanes
  const auto &lanes = j["lanes"];
  const std::string laneNames[] = {"Top", "Mid", "Bot"};
  for (int i = 0; i < 3; ++i) {
    if (!lanes.contains(laneNames[i]))
      continue;

    const auto &laneData = lanes[laneNames[i]];
    team.lanes[i].type = static_cast<LaneType>(i);
    team.lanes[i].width = laneData.value("width", 12.0f);

    for (const auto &wp : laneData["waypoints"]) {
      team.lanes[i].waypoints.push_back(ParseVec3(wp));
    }
  }

  return team;
}

// ── Team 2 Generation (Mirror) ──────────────────────────────────────────────

static void GenerateTeam2(MapData &data) {
  const auto &t1 = data.teams[0];
  auto &t2 = data.teams[1];

  // Mirror base
  t2.base = MapSymmetry::MirrorBase(t1.base, data.mapCenter);

  // Mirror towers
  t2.towers.clear();
  t2.towers.reserve(t1.towers.size());
  for (const auto &tower : t1.towers) {
    t2.towers.push_back(MapSymmetry::MirrorTower(tower, data.mapCenter));
  }

  // Mirror inhibitors
  t2.inhibitors.clear();
  t2.inhibitors.reserve(t1.inhibitors.size());
  for (const auto &inhib : t1.inhibitors) {
    t2.inhibitors.push_back(
        MapSymmetry::MirrorInhibitor(inhib, data.mapCenter));
  }

  // Mirror lanes (waypoints reversed)
  for (int i = 0; i < 3; ++i) {
    t2.lanes[i].type = t1.lanes[i].type;
    t2.lanes[i].width = t1.lanes[i].width;
    t2.lanes[i].waypoints =
        MapSymmetry::MirrorPath(t1.lanes[i].waypoints, data.mapCenter);
  }
}

// ── Public API ──────────────────────────────────────────────────────────────

MapData MapLoader::LoadFromFile(const std::string &filepath) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    throw std::runtime_error("MapLoader: cannot open file: " + filepath);
  }

  json root = json::parse(file);
  MapData data;

  data.mapName = root.value("mapName", "Unnamed Map");
  data.version = root.value("version", "0.0.0");

  // Team 1
  data.teams[0] = ParseTeam1(root["team1"]);

  // Generate Team 2 by mirroring
  GenerateTeam2(data);

  // Neutral camps
  if (root.contains("neutralCamps")) {
    for (const auto &camp : root["neutralCamps"]) {
      NeutralCamp nc;
      nc.position = ParseVec3(camp["position"]);
      nc.campType = ParseCampType(camp["campType"].get<std::string>());
      nc.spawnTime = camp.value("spawnTime", 90.0f);
      nc.respawnTime = camp.value("respawnTime", 300.0f);
      nc.leashRadius = camp.value("leashRadius", 8.0f);

      if (camp.contains("mobPositions")) {
        for (const auto &mp : camp["mobPositions"]) {
          nc.mobPositions.push_back(ParseVec3(mp));
        }
      }

      data.neutralCamps.push_back(nc);
    }
  }

  // Brush zones
  if (root.contains("brushZones")) {
    for (const auto &bz : root["brushZones"]) {
      BrushZone zone;
      zone.center = ParseVec3(bz["center"]);
      zone.halfExtents = ParseVec3(bz["halfExtents"]);

      if (bz.contains("team2Override")) {
        zone.team2Override = ParseVec3(bz["team2Override"]);
      }

      data.brushZones.push_back(zone);
    }
  }

  // Walls
  if (root.contains("walls")) {
    for (const auto &w : root["walls"]) {
      WallSegment wall;
      wall.start = ParseVec3(w["start"]);
      wall.end = ParseVec3(w["end"]);
      wall.thickness = w.value("thickness", 1.0f);
      wall.height = w.value("height", 3.0f);

      data.walls.push_back(wall);
    }
  }

  return data;
}

// ── Validation ──────────────────────────────────────────────────────────────

bool MapLoader::Validate(const MapData &data, std::string &outErrors) {
  std::ostringstream errors;
  bool valid = true;

  auto checkBounds = [&](const glm::vec3 &pos, const std::string &label) {
    if (pos.x < data.mapBoundsMin.x || pos.x > data.mapBoundsMax.x ||
        pos.y < data.mapBoundsMin.y || pos.y > data.mapBoundsMax.y ||
        pos.z < data.mapBoundsMin.z || pos.z > data.mapBoundsMax.z) {
      errors << "Out of bounds: " << label << " (" << pos.x << ", " << pos.y
             << ", " << pos.z << ")\n";
      valid = false;
    }
  };

  for (int t = 0; t < 2; ++t) {
    const auto &team = data.teams[t];
    std::string teamName = (t == 0) ? "Blue" : "Red";

    // Tower count: 3 lanes × 3 tiers + 2 nexus = 11
    if (team.towers.size() != 11) {
      errors << teamName << " team has " << team.towers.size()
             << " towers (expected 11)\n";
      valid = false;
    }

    // Inhibitor count: 3
    if (team.inhibitors.size() != 3) {
      errors << teamName << " team has " << team.inhibitors.size()
             << " inhibitors (expected 3)\n";
      valid = false;
    }

    // Check tower positions
    for (size_t i = 0; i < team.towers.size(); ++i) {
      checkBounds(team.towers[i].position,
                  teamName + " Tower " + std::to_string(i));
    }

    // Check inhibitor positions
    for (size_t i = 0; i < team.inhibitors.size(); ++i) {
      checkBounds(team.inhibitors[i].position,
                  teamName + " Inhibitor " + std::to_string(i));
    }

    // Check base positions
    checkBounds(team.base.nexusPosition, teamName + " Nexus");
    checkBounds(team.base.spawnPlatformCenter, teamName + " SpawnPlatform");
    checkBounds(team.base.shopPosition, teamName + " Shop");

    // Waypoint ordering: first waypoint near own base, last near enemy base
    for (int l = 0; l < 3; ++l) {
      const auto &lane = team.lanes[l];
      if (lane.waypoints.size() < 2) {
        errors << teamName << " Lane " << l << " has fewer than 2 waypoints\n";
        valid = false;
        continue;
      }

      float firstDist =
          glm::length(lane.waypoints.front() - team.base.nexusPosition);
      float lastDist =
          glm::length(lane.waypoints.back() - team.base.nexusPosition);

      if (firstDist > lastDist) {
        errors << teamName << " Lane " << l
               << " waypoints not ordered correctly "
               << "(first point should be near own base)\n";
        valid = false;
      }
    }
  }

  // Check neutral camp positions
  for (size_t i = 0; i < data.neutralCamps.size(); ++i) {
    checkBounds(data.neutralCamps[i].position,
                "NeutralCamp " + std::to_string(i));
  }

  // Check for duplicate positions (tolerance 0.5 units)
  {
    std::vector<std::pair<glm::vec3, std::string>> allPositions;

    for (int t = 0; t < 2; ++t) {
      std::string teamName = (t == 0) ? "Blue" : "Red";
      for (size_t i = 0; i < data.teams[t].towers.size(); ++i) {
        allPositions.push_back({data.teams[t].towers[i].position,
                                teamName + "_Tower_" + std::to_string(i)});
      }
    }

    const float tolerance = 0.5f;
    for (size_t i = 0; i < allPositions.size(); ++i) {
      for (size_t j = i + 1; j < allPositions.size(); ++j) {
        float dist = glm::length(allPositions[i].first - allPositions[j].first);
        if (dist < tolerance) {
          errors << "Duplicate position: " << allPositions[i].second << " and "
                 << allPositions[j].second << " (distance: " << dist << ")\n";
          valid = false;
        }
      }
    }
  }

  // Check neutral camps don't overlap with towers
  for (size_t c = 0; c < data.neutralCamps.size(); ++c) {
    for (int t = 0; t < 2; ++t) {
      for (size_t tw = 0; tw < data.teams[t].towers.size(); ++tw) {
        float dist = glm::length(data.neutralCamps[c].position -
                                 data.teams[t].towers[tw].position);
        if (dist < 2.0f) {
          errors << "Neutral camp " << c << " overlaps with "
                 << ((t == 0) ? "Blue" : "Red") << " tower " << tw
                 << " (distance: " << dist << ")\n";
          valid = false;
        }
      }
    }
  }

  outErrors = errors.str();
  return valid;
}

} // namespace glory
