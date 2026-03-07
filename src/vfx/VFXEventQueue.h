#pragma once

#include "vfx/VFXTypes.h"

#include <atomic>
#include <array>
#include <cstdint>

namespace glory {

// ── Lock-free SPSC ring buffer ─────────────────────────────────────────────
// Single-Producer Single-Consumer — game logic thread pushes, render thread pops.
// Capacity must be a power of two; the buffer holds Capacity-1 events maximum.
//
// Memory ordering: producer uses release on head after write (so consumer sees
// the completed write when it observes the new head).  Consumer uses release on
// tail after read (so producer sees freed slots).  Loads use acquire.
template<uint32_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power-of-two");
    static constexpr uint32_t MASK = Capacity - 1;

public:
    // Push from the producer thread. Returns false if the queue is full.
    bool push(const VFXEvent& ev) noexcept {
        const uint32_t head = m_head.load(std::memory_order_relaxed);
        const uint32_t next = (head + 1) & MASK;
        if (next == m_tail.load(std::memory_order_acquire)) return false; // full
        m_buffer[head] = ev;
        m_head.store(next, std::memory_order_release);
        return true;
    }

    // Pop from the consumer thread. Returns false if the queue is empty.
    bool pop(VFXEvent& out) noexcept {
        const uint32_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) return false; // empty
        out = m_buffer[tail];
        m_tail.store((tail + 1) & MASK, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return m_tail.load(std::memory_order_acquire) ==
               m_head.load(std::memory_order_acquire);
    }

private:
    std::array<VFXEvent, Capacity> m_buffer{};

    // Pad to separate cache lines to avoid false sharing between producer/consumer
    alignas(64) std::atomic<uint32_t> m_head{0};
    alignas(64) std::atomic<uint32_t> m_tail{0};
};

// 256-slot queue is sufficient for one frame's worth of ability VFX events
using VFXEventQueue = SPSCQueue<256>;

} // namespace glory
