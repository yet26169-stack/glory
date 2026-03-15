#pragma once

// ── GPU Timestamp Query Profiler ──────────────────────────────────────────────
//
// Allocates a VkQueryPool per frame-in-flight.  Each zone writes a pair of
// timestamps (begin + end).  At the start of the *next* frame the previous
// frame's pool is resolved into millisecond timings via timestampPeriod.
//
// Usage:
//   gpuTimer.resetFrame(cmd, frameIndex);          // after vkBeginCommandBuffer
//   gpuTimer.beginZone(cmd, frameIndex, "Shadow");
//   ...
//   gpuTimer.endZone(cmd, frameIndex, "Shadow");
//   auto results = gpuTimer.resolve(frameIndex);   // after vkWaitForFences
// ──────────────────────────────────────────────────────────────────────────────

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace glory {

struct GpuTimingResult {
    const char* name = nullptr;
    float       ms   = 0.0f;
};

class GpuTimer {
public:
    // maxZones = maximum concurrent begin/end pairs per frame.
    GpuTimer(VkDevice device, VkPhysicalDevice physDevice,
             uint32_t framesInFlight, uint32_t maxZones = 32);
    ~GpuTimer();

    GpuTimer(const GpuTimer&)            = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    // Call once at the top of each command buffer (after vkBeginCommandBuffer).
    // Resets all queries for this frame's pool.
    void resetFrame(VkCommandBuffer cmd, uint32_t frameIndex);

    // Record a begin-timestamp for a named zone.
    void beginZone(VkCommandBuffer cmd, uint32_t frameIndex, const char* name);

    // Record an end-timestamp for a named zone.
    void endZone(VkCommandBuffer cmd, uint32_t frameIndex, const char* name);

    // Resolve results from a COMPLETED frame (call after vkWaitForFences).
    // Returns per-zone timings in ms.  Order matches insertion order.
    const std::vector<GpuTimingResult>& resolve(uint32_t frameIndex);

    // Total GPU frame time (sum of all zones) for the last resolved frame.
    float totalMs(uint32_t frameIndex) const { return m_totalMs[frameIndex]; }

    // Returns true if the device supports timestamp queries.
    bool isSupported() const { return m_supported; }

private:
    VkDevice  m_device    = VK_NULL_HANDLE;
    float     m_nsTick    = 0.0f;       // timestampPeriod (nanoseconds per tick)
    bool      m_supported = false;
    uint32_t  m_maxZones  = 0;
    uint32_t  m_framesInFlight = 0;

    // Per-frame state
    struct FrameState {
        VkQueryPool pool = VK_NULL_HANDLE;
        uint32_t    nextQuery = 0;                              // next free query index
        std::vector<const char*> zoneNames;                     // index = zone id
        std::unordered_map<std::string, uint32_t> nameToZone;   // name → zone id
    };
    std::vector<FrameState>            m_frames;
    std::vector<float>                 m_totalMs;
    std::vector<std::vector<GpuTimingResult>> m_results;
};

} // namespace glory
