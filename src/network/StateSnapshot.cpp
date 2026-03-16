#include "network/StateSnapshot.h"
#include "scene/Components.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace glory {

// ── Serialization helpers ───────────────────────────────────────────────────

static void writeBytes(std::vector<uint8_t>& buf, const void* src, size_t n) {
    const auto* p = static_cast<const uint8_t*>(src);
    buf.insert(buf.end(), p, p + n);
}

template<typename T>
static void writeVal(std::vector<uint8_t>& buf, const T& val) {
    writeBytes(buf, &val, sizeof(T));
}

template<typename T>
static T readVal(const uint8_t* buf, size_t& offset) {
    T val;
    std::memcpy(&val, buf + offset, sizeof(T));
    offset += sizeof(T);
    return val;
}

// Serialize a component pool: [count][entity0, comp0, entity1, comp1, ...]
template<typename T>
void StateSnapshot::serializePool(const entt::registry& reg, std::vector<uint8_t>& buf) const {
    auto view = reg.view<T>();
    uint32_t count = static_cast<uint32_t>(view.size());
    writeVal(buf, count);
    for (auto entity : view) {
        uint32_t e = static_cast<uint32_t>(entity);
        writeVal(buf, e);
        const T& comp = view.template get<T>(entity);
        writeBytes(buf, &comp, sizeof(T));
    }
}

template<typename T>
size_t StateSnapshot::deserializePool(entt::registry& reg, const uint8_t* buf, size_t offset) const {
    uint32_t count = readVal<uint32_t>(buf, offset);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t e = readVal<uint32_t>(buf, offset);
        T comp;
        std::memcpy(&comp, buf + offset, sizeof(T));
        offset += sizeof(T);

        auto entity = static_cast<entt::entity>(e);
        if (!reg.valid(entity)) {
            entity = reg.create(entity);
        }
        reg.emplace_or_replace<T>(entity, comp);
    }
    return offset;
}

// ── StateSnapshot ───────────────────────────────────────────────────────────

void StateSnapshot::capture(const entt::registry& reg) {
    data.clear();
    data.reserve(4096);

    // Serialize all gameplay-relevant components (POD types only)
    serializePool<TransformComponent>(reg, data);
    serializePool<UnitComponent>(reg, data);
    serializePool<CharacterComponent>(reg, data);

    checksum = computeChecksum();

    spdlog::trace("[StateSnapshot] capture: tick={}, size={} bytes, checksum=0x{:08X}",
                  tick, data.size(), checksum);
}

void StateSnapshot::restore(entt::registry& reg) const {
    if (data.empty()) {
        spdlog::warn("[StateSnapshot] restore: empty snapshot for tick={}", tick);
        return;
    }

    size_t offset = 0;

    // Restore in same order as capture
    offset = deserializePool<TransformComponent>(reg, data.data(), offset);
    offset = deserializePool<UnitComponent>(reg, data.data(), offset);
    offset = deserializePool<CharacterComponent>(reg, data.data(), offset);

    spdlog::info("[StateSnapshot] restore: tick={}, read {} / {} bytes", tick, offset, data.size());
}

uint32_t StateSnapshot::computeChecksum() const {
    // FNV-1a hash over snapshot data
    uint32_t hash = 2166136261u;
    for (uint8_t byte : data) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

// ── RollbackManager ─────────────────────────────────────────────────────────

void RollbackManager::saveState(uint32_t tick, const entt::registry& reg) {
    auto& slot = m_history[m_head % ROLLBACK_WINDOW];
    slot.tick = tick;
    slot.capture(reg);

    m_head = (m_head + 1) % ROLLBACK_WINDOW;
    if (m_count < ROLLBACK_WINDOW) ++m_count;
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
    return false;
}

uint32_t RollbackManager::getChecksum(uint32_t tick) const {
    for (uint32_t i = 0; i < m_count; ++i) {
        uint32_t idx = (m_head + ROLLBACK_WINDOW - 1 - i) % ROLLBACK_WINDOW;
        if (m_history[idx].tick == tick) return m_history[idx].checksum;
    }
    return 0;
}

void RollbackManager::resimulate(uint32_t fromTick, uint32_t toTick, float dt,
                                 entt::registry& reg, ResimCallback cb) {
    if (!rollbackTo(fromTick, reg)) return;

    spdlog::info("[RollbackManager] resimulating ticks {} → {}", fromTick, toTick);
    for (uint32_t t = fromTick; t < toTick; ++t) {
        cb(reg, t, dt);
        saveState(t + 1, reg);
    }
}

} // namespace glory
