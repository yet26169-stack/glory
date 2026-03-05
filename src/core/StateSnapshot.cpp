#include "core/StateSnapshot.h"
#include "core/StateChecksumSystem.h"
#include "scene/Components.h"
#include <cstring>

namespace glory {

// ── Serialisation helpers ─────────────────────────────────────────────────────

template<typename T>
static void appendBytes(std::vector<uint8_t>& buf, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    buf.insert(buf.end(), p, p + sizeof(T));
}

template<typename T>
static size_t readBytes(const std::vector<uint8_t>& buf, size_t offset, T& out) {
    if (offset + sizeof(T) > buf.size()) return offset;
    std::memcpy(&out, buf.data() + offset, sizeof(T));
    return offset + sizeof(T);
}

// ── StateSnapshot ─────────────────────────────────────────────────────────────

void StateSnapshot::capture(const entt::registry& reg, uint32_t tickN) {
    tick = tickN;
    data.clear();

    // Serialise entity IDs + SimPosition
    reg.view<const SimPosition>().each([&](entt::entity e, const SimPosition& p) {
        uint32_t id = static_cast<uint32_t>(entt::to_integral(e));
        appendBytes(data, id);
        appendBytes(data, p);
    });

    // Serialise SimVelocity
    reg.view<const SimVelocity>().each([&](entt::entity e, const SimVelocity& v) {
        uint32_t id = static_cast<uint32_t>(entt::to_integral(e));
        appendBytes(data, id);
        appendBytes(data, v);
    });

    StateChecksumSystem scs;
    checksum = scs.compute(reg, tickN).hash;
}

void StateSnapshot::restore(entt::registry& reg) const {
    // Reconstruct SimPosition components from the serialised blob.
    // NOTE: This is a simplified restore — production code should handle
    // component-type tags in the stream and full entity lifecycle.
    size_t pos = 0;
    while (pos + sizeof(uint32_t) + sizeof(SimPosition) <= data.size()) {
        uint32_t id;
        SimPosition p;
        pos = readBytes(data, pos, id);
        pos = readBytes(data, pos, p);
        auto e = static_cast<entt::entity>(id);
        if (!reg.valid(e)) continue;
        reg.emplace_or_replace<SimPosition>(e, p);
    }
}

bool StateSnapshot::verify(const entt::registry& reg) const {
    StateChecksumSystem scs;
    return scs.compute(reg, tick).hash == checksum;
}

// ── SnapshotBuffer ────────────────────────────────────────────────────────────

void SnapshotBuffer::push(StateSnapshot snap) {
    m_buf[m_head % WINDOW] = std::move(snap);
    ++m_head;
}

const StateSnapshot* SnapshotBuffer::get(uint32_t tick) const {
    for (int i = 0; i < WINDOW; ++i) {
        if (m_buf[i].tick == tick) return &m_buf[i];
    }
    return nullptr;
}

} // namespace glory
