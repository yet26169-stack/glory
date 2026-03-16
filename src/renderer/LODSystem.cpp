#include "renderer/LODSystem.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cfloat>
#include <fstream>

namespace glory {

void LODSystem::loadConfig(const std::string& configPath) {
    std::ifstream f(configPath);
    if (!f.is_open()) {
        spdlog::info("[LODSystem] No config at '{}' — using built-in defaults", configPath);
        return;
    }

    try {
        auto j = nlohmann::json::parse(f);

        auto readPreset = [&](const std::string& name, LODConfig& cfg) {
            if (!j.contains(name)) return;
            auto& p = j[name];
            if (p.contains("lod1Distance"))     cfg.lod1Distance     = p["lod1Distance"].get<float>();
            if (p.contains("lod2Distance"))     cfg.lod2Distance     = p["lod2Distance"].get<float>();
            if (p.contains("lod3Distance"))     cfg.lod3Distance     = p["lod3Distance"].get<float>();
            if (p.contains("impostorDistance")) cfg.impostorDistance = p["impostorDistance"].get<float>();
        };

        readPreset("performance", m_configs[0]);
        readPreset("high_quality", m_configs[1]);
        readPreset("ultra",        m_configs[2]);

        spdlog::info("[LODSystem] Config loaded from '{}'", configPath);
    } catch (const std::exception& e) {
        spdlog::warn("[LODSystem] Failed to parse '{}': {}", configPath, e.what());
    }
}

void LODSystem::registerChain(uint32_t modelIndex, uint32_t subMeshIndex,
                               const LODChain& chain) {
    uint32_t k = key(modelIndex, subMeshIndex);
    if (k >= m_chains.size()) {
        m_chains.resize(k + 1);
    }
    m_chains[k] = chain;
    ++m_totalChains;
}

int LODSystem::selectLOD(uint32_t modelIndex, uint32_t subMeshIndex,
                          float distance) const {
    uint32_t k = key(modelIndex, subMeshIndex);
    if (k >= m_chains.size() || m_chains[k].levels.empty()) {
        // No chain registered — always LOD 0
        return shouldBeImpostor(distance) ? -1 : 0;
    }

    const auto& chain = m_chains[k];
    for (int i = 0; i < static_cast<int>(chain.levels.size()); ++i) {
        if (distance <= chain.levels[i].maxDistance) {
            return i;
        }
    }
    // Beyond all LOD levels — impostor
    return -1;
}

LODLevel LODSystem::getLODLevel(uint32_t modelIndex, uint32_t subMeshIndex,
                                 float distance) const {
    uint32_t k = key(modelIndex, subMeshIndex);
    if (k >= m_chains.size() || m_chains[k].levels.empty()) {
        return {}; // caller falls back to base MeshHandle
    }

    const auto& chain = m_chains[k];
    for (const auto& lod : chain.levels) {
        if (distance <= lod.maxDistance) {
            return lod;
        }
    }
    // Beyond furthest — return coarsest LOD (caller checks shouldBeImpostor
    // separately and may skip drawing the mesh entirely)
    return chain.levels.back();
}

bool LODSystem::shouldBeImpostor(float distance) const {
    return distance > m_configs[m_quality].impostorDistance;
}

} // namespace glory
