#pragma once
/// Ring-buffer pool of Vulkan staging buffers for CPU → GPU uploads.
///
/// Instead of creating a new staging buffer for every texture/mesh upload,
/// StagingPool pre-allocates a set of large staging buffers and hands out
/// sub-allocations.  Buffers are reclaimed automatically once the GPU frame
/// that consumed them has completed (tracked via timeline semaphore values).
///
/// Usage:
///   auto region = stagingPool.acquire(uploadSize);
///   memcpy(region.mapped, cpuData, uploadSize);
///   stagingPool.flush(region);
///   // record vkCmdCopyBufferToImage / vkCmdCopyBuffer using region.buffer + region.offset
///   stagingPool.markInFlight(region, currentTimelineValue);
///
///   // Each frame start:
///   stagingPool.reclaimCompleted(completedTimelineValue);

#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace glory {

class Device;

/// A sub-allocation from the staging ring buffer.
struct StagingRegion {
    VkBuffer     buffer   = VK_NULL_HANDLE;
    VkDeviceSize offset   = 0;
    VkDeviceSize size     = 0;
    void*        mapped   = nullptr;    // CPU-visible pointer (buffer base + offset)
    uint32_t     slotIndex = UINT32_MAX; // internal: which slot this came from
};

class StagingPool {
public:
    /// Configuration for the pool.
    struct Config {
        uint32_t     slotCount;
        VkDeviceSize slotSize;
        Config() : slotCount(8), slotSize(4 * 1024 * 1024) {}
    };

    StagingPool() = default;

    /// Initialise the pool.  Call once during renderer setup.
    void init(VmaAllocator allocator, Config cfg = Config());

    /// Destroy all Vulkan resources.
    void destroy();

    /// Acquire a staging region large enough for `size` bytes.
    /// Falls back to a one-shot oversized buffer if no slot has room.
    StagingRegion acquire(VkDeviceSize size, VkDeviceSize alignment = 256);

    /// Flush the CPU writes in a region (calls vmaFlushAllocation).
    void flush(const StagingRegion& region);

    /// Mark a slot as in-flight with a timeline semaphore value.
    /// The slot won't be reused until that value has been signalled.
    void markInFlight(const StagingRegion& region, uint64_t timelineValue);

    /// Reclaim any slots whose timeline value ≤ completedValue.
    /// Call at the start of each frame after waitForFrame().
    void reclaimCompleted(uint64_t completedValue);

    /// Statistics.
    struct Stats {
        size_t       totalSlots       = 0;
        size_t       freeSlots        = 0;
        size_t       inFlightSlots    = 0;
        VkDeviceSize totalCapacity    = 0;
        VkDeviceSize peakUsage        = 0;
        uint32_t     oversizedAllocs  = 0;  // fallback one-shots
    };
    Stats getStats() const;

private:
    struct Slot {
        Buffer       buffer;
        void*        mappedBase   = nullptr;
        VkDeviceSize offset       = 0;     // bump pointer within the slot
        VkDeviceSize capacity     = 0;
        uint64_t     timelineValue = 0;    // 0 = free
        bool         inFlight      = false;
    };

    VmaAllocator       m_allocator = VK_NULL_HANDLE;
    std::vector<Slot>  m_slots;
    uint32_t           m_currentSlot  = 0;
    VkDeviceSize       m_slotSize     = 0;
    uint32_t           m_oversizedCount = 0;
    VkDeviceSize       m_peakUsage    = 0;
    VkDeviceSize       m_currentUsage = 0;

    // Oversized one-shot buffers (destroyed after reclaim)
    struct OversizedBuffer {
        Buffer       buffer;
        uint64_t     timelineValue = 0;
    };
    std::vector<OversizedBuffer> m_oversized;

    uint32_t findFreeSlot(VkDeviceSize needed);
};

} // namespace glory
