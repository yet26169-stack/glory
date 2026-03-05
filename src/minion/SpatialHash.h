#pragma once

#include <entt.hpp>
#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace glory {

/// Uniform-grid spatial hash for fast neighbor queries.
/// Cell size should match the largest query radius (aggro range).
class SpatialHash {
public:
  explicit SpatialHash(float cellSize = 700.0f) : m_cellSize(cellSize) {}

  void clear() { m_cells.clear(); }

  void insert(entt::entity e, const glm::vec3 &pos) {
    auto key = cellKey(pos);
    m_cells[key].push_back({e, pos});
  }

  /// Query all entities within `radius` of `center`.
  /// Results written to `out` (cleared first).
  void query(const glm::vec3 &center, float radius,
             std::vector<entt::entity> &out) const {
    out.clear();
    float r2 = radius * radius;

    int minCX = static_cast<int>(std::floor((center.x - radius) / m_cellSize));
    int maxCX = static_cast<int>(std::floor((center.x + radius) / m_cellSize));
    int minCZ = static_cast<int>(std::floor((center.z - radius) / m_cellSize));
    int maxCZ = static_cast<int>(std::floor((center.z + radius) / m_cellSize));

    for (int cx = minCX; cx <= maxCX; ++cx) {
      for (int cz = minCZ; cz <= maxCZ; ++cz) {
        uint64_t key = packKey(cx, cz);
        auto it = m_cells.find(key);
        if (it == m_cells.end())
          continue;
        for (const auto &entry : it->second) {
          float dx = entry.pos.x - center.x;
          float dz = entry.pos.z - center.z;
          if (dx * dx + dz * dz <= r2) {
            out.push_back(entry.entity);
          }
        }
      }
    }
  }

private:
  struct Entry {
    entt::entity entity;
    glm::vec3 pos;
  };

  float m_cellSize;
  std::unordered_map<uint64_t, std::vector<Entry>> m_cells;

  uint64_t cellKey(const glm::vec3 &pos) const {
    int cx = static_cast<int>(std::floor(pos.x / m_cellSize));
    int cz = static_cast<int>(std::floor(pos.z / m_cellSize));
    return packKey(cx, cz);
  }

  static uint64_t packKey(int cx, int cz) {
    auto ux = static_cast<uint32_t>(cx + 32768);
    auto uz = static_cast<uint32_t>(cz + 32768);
    return (static_cast<uint64_t>(ux) << 32) | uz;
  }
};

} // namespace glory
