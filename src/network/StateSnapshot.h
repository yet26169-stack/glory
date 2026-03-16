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

    // Serialize all networked gameplay components from the registry
    void capture(const entt::registry& reg);

    // Restore all networked gameplay components into the registry
    void restore(entt::registry& reg) const;

    uint32_t computeChecksum() const;
    size_t sizeBytes() const { return data.size(); }

private:
    // Helpers to serialize/deserialize typed component pools
    template<typename T>
    void serializePool(const entt::registry& reg, std::vector<uint8_t>& buf) const;

    template<typename T>
    size_t deserializePool(entt::registry& reg, const uint8_t* buf, size_t offset) const;
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

    // Get checksum for a tick (for sending to remote)
    uint32_t getChecksum(uint32_t tick) const;

    // Re-simulate from a tick to currentTick using a callback
    using ResimCallback = void(*)(entt::registry&, uint32_t tick, float dt);
    void resimulate(uint32_t fromTick, uint32_t toTick, float dt,
                    entt::registry& reg, ResimCallback cb);

private:
    std::array<StateSnapshot, ROLLBACK_WINDOW> m_history;
    uint32_t m_head = 0;
    uint32_t m_count = 0;
};

} // namespace glory
