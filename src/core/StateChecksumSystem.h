#pragma once
#include <cstdint>
#include <entt.hpp>

namespace glory {

struct StateChecksum {
    uint64_t hash  = 0;
    uint32_t tickN = 0;
};

class StateChecksumSystem {
public:
    StateChecksum compute(const entt::registry& reg, uint32_t tick);
};

} // namespace glory
