#include "core/SystemScheduler.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <latch>
#include <stdexcept>
#include <unordered_map>

namespace glory {

void SystemScheduler::build() {
    const uint32_t N = static_cast<uint32_t>(m_systems.size());
    if (N == 0) { m_levels.clear(); m_dirty = false; return; }

    // Map type_index → system index for dependency lookup
    std::unordered_map<std::type_index, uint32_t> typeToIdx;
    for (uint32_t i = 0; i < N; ++i) {
        // Use the concrete type behind the ISystem pointer
        typeToIdx[std::type_index(typeid(*m_systems[i]))] = i;
    }

    // Build adjacency list + in-degree from declared dependencies
    std::vector<std::vector<uint32_t>> successors(N);
    std::vector<uint32_t> inDegree(N, 0);

    for (uint32_t i = 0; i < N; ++i) {
        for (const auto& dep : m_systems[i]->dependsOn()) {
            auto it = typeToIdx.find(dep);
            if (it == typeToIdx.end()) {
                spdlog::warn("SystemScheduler: {} depends on unregistered type, ignoring",
                             m_systems[i]->name());
                continue;
            }
            uint32_t depIdx = it->second;
            successors[depIdx].push_back(i);
            ++inDegree[i];
        }
    }

    // Kahn's algorithm: compute levels (BFS layers)
    m_levels.clear();
    std::vector<uint32_t> currentLevel;
    for (uint32_t i = 0; i < N; ++i) {
        if (inDegree[i] == 0) currentLevel.push_back(i);
    }

    uint32_t processed = 0;
    while (!currentLevel.empty()) {
        m_levels.push_back(currentLevel);
        processed += static_cast<uint32_t>(currentLevel.size());

        std::vector<uint32_t> nextLevel;
        for (uint32_t idx : currentLevel) {
            for (uint32_t succ : successors[idx]) {
                if (--inDegree[succ] == 0) {
                    nextLevel.push_back(succ);
                }
            }
        }
        currentLevel = std::move(nextLevel);
    }

    if (processed != N) {
        spdlog::error("SystemScheduler: dependency cycle detected! {} of {} systems scheduled.",
                      processed, N);
    }

    m_dirty = false;

    // Log the schedule
    for (size_t lvl = 0; lvl < m_levels.size(); ++lvl) {
        std::string names;
        for (uint32_t idx : m_levels[lvl]) {
            if (!names.empty()) names += ", ";
            names += m_systems[idx]->name();
        }
        spdlog::info("SystemScheduler level {}: [{}]", lvl, names);
    }
}

void SystemScheduler::tick(entt::registry& registry, float dt, ThreadPool& pool) {
    if (m_dirty) build();

    for (const auto& level : m_levels) {
        if (level.size() == 1) {
            // Single system — run directly, no threading overhead
            m_systems[level[0]]->execute(registry, dt);
        } else {
            // Multiple systems at this level — run in parallel
            std::latch done(static_cast<std::ptrdiff_t>(level.size()));
            for (uint32_t idx : level) {
                pool.submit([this, idx, &registry, dt, &done]() {
                    m_systems[idx]->execute(registry, dt);
                    done.count_down();
                });
            }
            done.wait();
        }
    }
}

} // namespace glory
