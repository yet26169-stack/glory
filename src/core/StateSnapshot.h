#pragma once
#include <cstdint>
#include <vector>
#include <entt.hpp>

namespace glory {

/// Binary blob of all Sim* component data for one simulation tick.
/// Used for rollback networking and replay verification.
struct StateSnapshot {
    uint32_t             tick     = 0;
    uint64_t             checksum = 0;
    std::vector<uint8_t> data;   // serialised Sim* component data (zstd-ready)

    void capture(const entt::registry& reg, uint32_t tickN);
    void restore(entt::registry& reg) const;
    /// Recompute hash from reg and compare to stored checksum.
    bool verify(const entt::registry& reg) const;
};

/// Ring buffer of recent snapshots for rollback window (default: 8 ticks).
class SnapshotBuffer {
public:
    static constexpr int WINDOW = 8;

    void push(StateSnapshot snap);
    const StateSnapshot* get(uint32_t tick) const;

private:
    StateSnapshot m_buf[WINDOW];
    uint32_t      m_head = 0;
};

} // namespace glory
