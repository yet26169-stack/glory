#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <entt.hpp>

namespace glory {

class StateSnapshot {
public:
    uint32_t tick = 0;
    uint32_t checksum = 0;
    std::vector<uint8_t> data;

    void capture(const entt::registry& reg);
    void restore(entt::registry& reg) const;
    uint32_t computeChecksum() const;
    size_t sizeBytes() const { return data.size(); }
};

static constexpr uint32_t ROLLBACK_WINDOW = 128;  // ~4.2 seconds at 30Hz

class RollbackManager {
public:
    void saveState(uint32_t tick, const entt::registry& reg);
    bool rollbackTo(uint32_t tick, entt::registry& reg);
    bool hasState(uint32_t tick) const;
    uint32_t oldestTick() const;
    uint32_t newestTick() const;

    // Checksum comparison for desync detection
    bool verifyChecksum(uint32_t tick, uint32_t remoteChecksum) const;

private:
    std::array<StateSnapshot, ROLLBACK_WINDOW> m_history;
    uint32_t m_head = 0;
    uint32_t m_count = 0;
};

} // namespace glory
