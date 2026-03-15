#include "fog/FogSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace glory {

FogSystem::FogSystem() {
    const uint32_t n = MAP_SIZE * MAP_SIZE;
    m_visionBuffer.resize(n, 0);
    m_explorationBuffer.resize(n, 0);
    m_visBuffer.resize(n, 0);
}

int FogSystem::worldToCell(float worldCoord) const {
    return static_cast<int>((worldCoord - WORLD_MIN) / WORLD_SIZE * static_cast<float>(MAP_SIZE));
}

void FogSystem::paintCircle(float worldX, float worldZ, float radius) {
    const float cellsPerUnit = static_cast<float>(MAP_SIZE) / WORLD_SIZE;
    const float rCells       = radius * cellsPerUnit;
    const int   cx           = worldToCell(worldX);
    const int   cz           = worldToCell(worldZ);
    const int   ri           = static_cast<int>(std::ceil(rCells)) + 1;
    const float r2           = rCells * rCells;

    for (int dz = -ri; dz <= ri; ++dz) {
        for (int dx = -ri; dx <= ri; ++dx) {
            const float dist2 = static_cast<float>(dx * dx + dz * dz);
            if (dist2 > r2) continue;

            const int gx = cx + dx;
            const int gz = cz + dz;
            if (gx < 0 || gx >= static_cast<int>(MAP_SIZE) ||
                gz < 0 || gz >= static_cast<int>(MAP_SIZE)) continue;

            // Soft falloff: full brightness at centre, fades to 0 at edge
            const float falloff = std::clamp(1.0f - std::sqrt(dist2) / rCells, 0.0f, 1.0f);
            // Sharpen: keeps the inner ~70% at full brightness
            const float sharpened = std::clamp(falloff * 3.0f - 1.0f, 0.0f, 1.0f);
            const uint8_t v = static_cast<uint8_t>(255.0f * sharpened);

            const uint32_t idx = static_cast<uint32_t>(gz) * MAP_SIZE +
                                 static_cast<uint32_t>(gx);
            m_visionBuffer[idx] = std::max(m_visionBuffer[idx], v);
        }
    }
}

void FogSystem::buildCombinedBuffer() {
    for (uint32_t i = 0; i < MAP_SIZE * MAP_SIZE; ++i) {
        if (m_visionBuffer[i] > 0) {
            m_visBuffer[i] = m_visionBuffer[i];   // fully visible
        } else if (m_explorationBuffer[i] > 0) {
            m_visBuffer[i] = m_explorationBuffer[i]; // shroud (explored but dark)
        } else {
            m_visBuffer[i] = 0;                    // unexplored black
        }
    }
}

void FogSystem::updateCpuOnly(const std::vector<VisionEntity>& entities) {
    std::fill(m_visionBuffer.begin(), m_visionBuffer.end(), 0);

    for (const auto& e : entities)
        paintCircle(e.position.x, e.position.z, e.sightRange);

    // Any pixel currently lit gets stamped into the exploration buffer at 127
    for (uint32_t i = 0; i < MAP_SIZE * MAP_SIZE; ++i) {
        if (m_visionBuffer[i] > 0)
            m_explorationBuffer[i] = 127;
    }

    buildCombinedBuffer();
}

void FogSystem::update(const std::vector<VisionEntity>& entities) {
    updateCpuOnly(entities);
}

bool FogSystem::isPositionVisible(float worldX, float worldZ) const {
    const int x = worldToCell(worldX);
    const int z = worldToCell(worldZ);
    if (x < 0 || x >= static_cast<int>(MAP_SIZE) ||
        z < 0 || z >= static_cast<int>(MAP_SIZE)) return false;
    return m_visionBuffer[static_cast<uint32_t>(z) * MAP_SIZE +
                          static_cast<uint32_t>(x)] > 0;
}

bool FogSystem::isPositionExplored(float worldX, float worldZ) const {
    const int x = worldToCell(worldX);
    const int z = worldToCell(worldZ);
    if (x < 0 || x >= static_cast<int>(MAP_SIZE) ||
        z < 0 || z >= static_cast<int>(MAP_SIZE)) return false;
    return m_explorationBuffer[static_cast<uint32_t>(z) * MAP_SIZE +
                               static_cast<uint32_t>(x)] > 0;
}

} // namespace glory
