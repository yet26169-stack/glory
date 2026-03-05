#pragma once
#include <cstdint>
#include <cstddef>
#include <entt.hpp>

namespace glory {

void MurmurHash3_x64_128(const void* key, int len, uint32_t seed, void* out);

/// Hash all Sim* component data from the registry.
uint64_t hashSimState(const entt::registry& reg);

} // namespace glory
