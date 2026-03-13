#include "network/StateSnapshot.h"
#include <spdlog/spdlog.h>
#include <numeric>

namespace glory {

// --- StateSnapshot ---

void StateSnapshot::capture(const entt::registry& reg) {
    // Stub: in a real implementation, serialize all networked components
    data.clear();
    auto view = reg.storage();
    size_t entityCount = 0;
    for ([[maybe_unused]] auto&& [id, storage] : view) {
        entityCount += storage.size();
    }

    // Store a placeholder byte per entity to simulate snapshot size
    data.resize(entityCount, 0);
    checksum = computeChecksum();

    spdlog::trace("[StateSnapshot] capture: tick={}, entities~={}, size={} bytes",
                  tick, entityCount, data.size());
}

void StateSnapshot::restore(entt::registry& reg) const {
    // Stub: in a real implementation, deserialize all networked components
    spdlog::info("[StateSnapshot] restore stub: tick={}, size={} bytes", tick, data.size());
    (void)reg;
}

uint32_t StateSnapshot::computeChecksum() const {
    // Simple FNV-1a hash over snapshot data
    uint32_t hash = 2166136261u;
    for (uint8_t byte : data) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

// --- RollbackManager ---

void RollbackManager::saveState(uint32_t tick, const entt::registry& reg) {
    auto& slot = m_history[m_head % ROLLBACK_WINDOW];
    slot.tick = tick;
    slot.capture(reg);

    m_head = (m_head + 1) % ROLLBACK_WINDOW;
    if (m_count < ROLLBACK_WINDOW) ++m_count;

    spdlog::trace("[RollbackManager] saveState: tick={}, slots used={}", tick, m_count);
}

bool RollbackManager::rollbackTo(uint32_t tick, entt::registry& reg) {
    for (uint32_t i = 0; i < m_count; ++i) {
        uint32_t idx = (m_head + ROLLBACK_WINDOW - 1 - i) % ROLLBACK_WINDOW;
        if (m_history[idx].tick == tick) {
            m_history[idx].restore(reg);
            spdlog::info("[RollbackManager] rollbackTo: tick={}", tick);
            return true;
        }
    }
    spdlog::warn("[RollbackManager] rollbackTo: tick={} not found", tick);
    return false;
}

bool RollbackManager::hasState(uint32_t tick) const {
    for (uint32_t i = 0; i < m_count; ++i) {
        uint32_t idx = (m_head + ROLLBACK_WINDOW - 1 - i) % ROLLBACK_WINDOW;
        if (m_history[idx].tick == tick) return true;
    }
    return false;
}

uint32_t RollbackManager::oldestTick() const {
    if (m_count == 0) return 0;
    uint32_t idx = (m_head + ROLLBACK_WINDOW - m_count) % ROLLBACK_WINDOW;
    return m_history[idx].tick;
}

uint32_t RollbackManager::newestTick() const {
    if (m_count == 0) return 0;
    uint32_t idx = (m_head + ROLLBACK_WINDOW - 1) % ROLLBACK_WINDOW;
    return m_history[idx].tick;
}

bool RollbackManager::verifyChecksum(uint32_t tick, uint32_t remoteChecksum) const {
    for (uint32_t i = 0; i < m_count; ++i) {
        uint32_t idx = (m_head + ROLLBACK_WINDOW - 1 - i) % ROLLBACK_WINDOW;
        if (m_history[idx].tick == tick) {
            bool match = (m_history[idx].checksum == remoteChecksum);
            if (!match) {
                spdlog::warn("[RollbackManager] DESYNC at tick={}: local=0x{:08X} remote=0x{:08X}",
                             tick, m_history[idx].checksum, remoteChecksum);
            }
            return match;
        }
    }
    spdlog::warn("[RollbackManager] verifyChecksum: tick={} not found", tick);
    return false;
}

} // namespace glory
