#include "core/StateChecksumSystem.h"
#include "math/MurmurHash3.h"

namespace glory {

StateChecksum StateChecksumSystem::compute(const entt::registry& reg, uint32_t tick) {
    StateChecksum cs;
    cs.tickN = tick;
    cs.hash  = hashSimState(reg);
    return cs;
}

} // namespace glory
