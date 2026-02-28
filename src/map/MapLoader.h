#pragma once

#include "map/MapTypes.h"

#include <string>

namespace glory {

class MapLoader {
public:
  /// Load from JSON file path. Returns fully populated MapData with both teams.
  static MapData LoadFromFile(const std::string &filepath);

  /// Validate loaded data: checks bounds, tower counts, waypoint ordering.
  static bool Validate(const MapData &data, std::string &outErrors);
};

} // namespace glory
