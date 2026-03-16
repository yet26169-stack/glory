#include "renderer/StagingPool.h"

#include <spdlog/spdlog.h>
#include <cassert>
#include <algorithm>

namespace glory {

void StagingPool::init(VmaAllocator allocator, Config cfg) {
    m_allocator = allocator;
    m_slotSize  = cfg.slotSize;

    m_slots.resize(cfg.slotCount);
    for (auto& slot : m_slots) {
        slot.buffer = Buffer(allocator, cfg.slotSize,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU);
        slot.mappedBase = slot.buffer.map();
        slot.capacity   = cfg.slotSize;
        slot.offset     = 0;
        slot.inFlight   = false;
        slot.timelineValue = 0;
    }

    spdlog::info("StagingPool: {} slots × {} MB = {} MB total",
                 cfg.slotCount, cfg.slotSize / (1024 * 1024),
                 (cfg.slotCount * cfg.slotSize) / (1024 * 1024));
}

void StagingPool::destroy() {
    for (auto& slot : m_slots) {
        if (slot.mappedBase) {
            slot.buffer.unmap();
            slot.mappedBase = nullptr;
        }
        slot.buffer.destroy();
    }
    m_slots.clear();

    for (auto& os : m_oversized) {
        os.buffer.destroy();
    }
    m_oversized.clear();
}

uint32_t StagingPool::findFreeSlot(VkDeviceSize needed) {
    // First try the current slot
    if (m_currentSlot < m_slots.size()) {
        auto& s = m_slots[m_currentSlot];
        if (!s.inFlight && (s.offset + needed <= s.capacity))
            return m_currentSlot;
    }

    // Search for any free slot with enough room
    for (uint32_t i = 0; i < m_slots.size(); ++i) {
        auto& s = m_slots[i];
        if (!s.inFlight && (s.offset + needed <= s.capacity))
            return i;
    }

    // Search for any free slot (reset offset first)
    for (uint32_t i = 0; i < m_slots.size(); ++i) {
        auto& s = m_slots[i];
        if (!s.inFlight) {
            s.offset = 0;
            if (needed <= s.capacity)
                return i;
        }
    }

    return UINT32_MAX; // all in-flight or too small
}

StagingRegion StagingPool::acquire(VkDeviceSize size, VkDeviceSize alignment) {
    // Align the offset
    auto alignUp = [](VkDeviceSize v, VkDeviceSize a) -> VkDeviceSize {
        return (v + a - 1) & ~(a - 1);
    };

    // Try to sub-allocate from the pool
    if (size <= m_slotSize) {
        uint32_t idx = findFreeSlot(size);
        if (idx != UINT32_MAX) {
            auto& slot = m_slots[idx];
            VkDeviceSize aligned = alignUp(slot.offset, alignment);
            if (aligned + size <= slot.capacity) {
                StagingRegion region{};
                region.buffer    = slot.buffer.getBuffer();
                region.offset    = aligned;
                region.size      = size;
                region.mapped    = static_cast<uint8_t*>(slot.mappedBase) + aligned;
                region.slotIndex = idx;

                slot.offset = aligned + size;
                m_currentSlot = idx;

                m_currentUsage += size;
                if (m_currentUsage > m_peakUsage) m_peakUsage = m_currentUsage;

                return region;
            }
        }
    }

    // Fallback: oversized one-shot buffer
    ++m_oversizedCount;
    spdlog::debug("StagingPool: oversized alloc {} KB", size / 1024);

    OversizedBuffer os;
    os.buffer = Buffer(m_allocator, size,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VMA_MEMORY_USAGE_CPU_TO_GPU);
    void* mapped = os.buffer.map();

    StagingRegion region{};
    region.buffer    = os.buffer.getBuffer();
    region.offset    = 0;
    region.size      = size;
    region.mapped    = mapped;
    region.slotIndex = UINT32_MAX; // marks as oversized

    m_oversized.push_back(std::move(os));
    return region;
}

void StagingPool::flush(const StagingRegion& region) {
    if (region.slotIndex < m_slots.size()) {
        m_slots[region.slotIndex].buffer.flush();
    } else if (!m_oversized.empty()) {
        m_oversized.back().buffer.flush();
    }
}

void StagingPool::markInFlight(const StagingRegion& region, uint64_t timelineValue) {
    if (region.slotIndex < m_slots.size()) {
        auto& slot = m_slots[region.slotIndex];
        slot.inFlight = true;
        slot.timelineValue = timelineValue;
    } else if (!m_oversized.empty()) {
        m_oversized.back().timelineValue = timelineValue;
    }
}

void StagingPool::reclaimCompleted(uint64_t completedValue) {
    for (auto& slot : m_slots) {
        if (slot.inFlight && slot.timelineValue <= completedValue) {
            slot.inFlight = false;
            slot.offset   = 0;
            slot.timelineValue = 0;
            m_currentUsage = (m_currentUsage > slot.capacity)
                           ? m_currentUsage - slot.capacity : 0;
        }
    }

    // Destroy completed oversized buffers
    auto it = std::remove_if(m_oversized.begin(), m_oversized.end(),
        [completedValue](OversizedBuffer& os) {
            if (os.timelineValue <= completedValue && os.timelineValue != 0) {
                os.buffer.unmap();
                os.buffer.destroy();
                return true;
            }
            return false;
        });
    m_oversized.erase(it, m_oversized.end());
}

StagingPool::Stats StagingPool::getStats() const {
    Stats st{};
    st.totalSlots      = m_slots.size();
    st.totalCapacity   = m_slots.size() * m_slotSize;
    st.peakUsage       = m_peakUsage;
    st.oversizedAllocs = m_oversizedCount;

    for (auto& s : m_slots) {
        if (s.inFlight) ++st.inFlightSlots;
        else            ++st.freeSlots;
    }
    return st;
}

} // namespace glory
