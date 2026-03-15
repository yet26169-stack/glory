#include "renderer/GpuTimer.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <spdlog/spdlog.h>

namespace glory {

GpuTimer::GpuTimer(VkDevice device, VkPhysicalDevice physDevice,
                   uint32_t framesInFlight, uint32_t maxZones)
    : m_device(device)
    , m_maxZones(maxZones)
    , m_framesInFlight(framesInFlight)
{
    // Query timestamp support
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physDevice, &props);
    m_nsTick = props.limits.timestampPeriod;  // nanoseconds per tick

    // timestampPeriod == 0 means timestamps are not supported
    if (m_nsTick == 0.0f) {
        spdlog::warn("GpuTimer: GPU does not support timestamp queries");
        m_supported = false;
        m_frames.resize(framesInFlight);
        m_totalMs.resize(framesInFlight, 0.0f);
        m_results.resize(framesInFlight);
        return;
    }
    m_supported = true;

    // Each zone needs 2 queries (begin + end).
    uint32_t queryCount = maxZones * 2;

    m_frames.resize(framesInFlight);
    m_totalMs.resize(framesInFlight, 0.0f);
    m_results.resize(framesInFlight);

    VkQueryPoolCreateInfo ci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    ci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = queryCount;

    for (uint32_t i = 0; i < framesInFlight; ++i) {
        VkResult result = vkCreateQueryPool(device, &ci, nullptr, &m_frames[i].pool);
        if (result != VK_SUCCESS) {
            spdlog::error("GpuTimer: Failed to create query pool for frame {}", i);
            m_supported = false;
            return;
        }
        m_frames[i].zoneNames.reserve(maxZones);
    }
    spdlog::info("GpuTimer: {} pools × {} queries (timestampPeriod = {:.2f} ns)",
                 framesInFlight, queryCount, m_nsTick);
}

GpuTimer::~GpuTimer() {
    for (auto& f : m_frames) {
        if (f.pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, f.pool, nullptr);
        }
    }
}

void GpuTimer::resetFrame(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!m_supported) return;
    auto& f = m_frames[frameIndex];
    vkCmdResetQueryPool(cmd, f.pool, 0, m_maxZones * 2);
    f.nextQuery = 0;
    f.zoneNames.clear();
    f.nameToZone.clear();
}

void GpuTimer::beginZone(VkCommandBuffer cmd, uint32_t frameIndex, const char* name) {
    if (!m_supported) return;
    auto& f = m_frames[frameIndex];
    uint32_t zoneId = static_cast<uint32_t>(f.zoneNames.size());
    if (zoneId >= m_maxZones) return;  // silently drop if we exceed capacity

    f.zoneNames.push_back(name);
    f.nameToZone[name] = zoneId;

    // begin timestamp = query index zoneId*2
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        f.pool, zoneId * 2);
}

void GpuTimer::endZone(VkCommandBuffer cmd, uint32_t frameIndex, const char* name) {
    if (!m_supported) return;
    auto& f = m_frames[frameIndex];
    auto it = f.nameToZone.find(name);
    if (it == f.nameToZone.end()) return;

    uint32_t zoneId = it->second;
    // end timestamp = query index zoneId*2 + 1
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        f.pool, zoneId * 2 + 1);
}

const std::vector<GpuTimingResult>& GpuTimer::resolve(uint32_t frameIndex) {
    auto& results = m_results[frameIndex];
    results.clear();
    m_totalMs[frameIndex] = 0.0f;

    if (!m_supported) return results;

    auto& f = m_frames[frameIndex];
    uint32_t zoneCount = static_cast<uint32_t>(f.zoneNames.size());
    if (zoneCount == 0) return results;

    // Read all timestamps at once
    uint32_t queryCount = zoneCount * 2;
    std::vector<uint64_t> timestamps(queryCount, 0);
    VkResult vr = vkGetQueryPoolResults(
        m_device, f.pool,
        0, queryCount,
        queryCount * sizeof(uint64_t), timestamps.data(), sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    if (vr != VK_SUCCESS) return results;

    float nsToMs = m_nsTick / 1'000'000.0f;
    float total  = 0.0f;

    results.resize(zoneCount);
    for (uint32_t i = 0; i < zoneCount; ++i) {
        uint64_t begin = timestamps[i * 2];
        uint64_t end   = timestamps[i * 2 + 1];
        float ms = (end >= begin) ? static_cast<float>(end - begin) * nsToMs : 0.0f;
        results[i].name = f.zoneNames[i];
        results[i].ms   = ms;
        total += ms;
    }
    m_totalMs[frameIndex] = total;
    return results;
}

} // namespace glory
