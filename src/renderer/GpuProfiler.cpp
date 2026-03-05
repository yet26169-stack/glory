#include "renderer/GpuProfiler.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace glory {

GpuProfiler::GpuProfiler(const Device& device, uint32_t framesInFlight)
    : m_device(device), m_framesInFlight(framesInFlight)
{
    m_results.fill(0.0f);

    // Check timestamp support
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device.getPhysicalDevice(), &props);
    if (props.limits.timestampComputeAndGraphics == VK_FALSE) {
        spdlog::warn("GpuProfiler: device does not support timestamp queries — profiling disabled");
        return;
    }
    if (props.limits.timestampPeriod == 0.0f) {
        spdlog::warn("GpuProfiler: timestampPeriod == 0 — profiling disabled");
        return;
    }
    m_periodNs  = props.limits.timestampPeriod; // nanoseconds per tick
    m_supported = true;

    VkDevice dev       = device.getDevice();
    VmaAllocator alloc = device.getAllocator();

    m_pools.resize(framesInFlight, VK_NULL_HANDLE);
    m_readbackBuffers.reserve(framesInFlight);
    m_frameWritten.resize(framesInFlight, false);

    for (uint32_t f = 0; f < framesInFlight; ++f) {
        // Query pool: 2 timestamps per pass (begin + end)
        VkQueryPoolCreateInfo qpCI{};
        qpCI.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpCI.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpCI.queryCount = STAMPS_PER_FRAME;
        VK_CHECK(vkCreateQueryPool(dev, &qpCI, nullptr, &m_pools[f]),
                 "Failed to create timestamp query pool");

        // Readback buffer: host-visible, uint64_t per query slot
        m_readbackBuffers.emplace_back(alloc, sizeof(uint64_t) * STAMPS_PER_FRAME,
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VMA_MEMORY_USAGE_GPU_TO_CPU);
    }
    spdlog::info("GpuProfiler initialized ({} frames, period {:.2f} ns/tick)",
                 framesInFlight, m_periodNs);
}

GpuProfiler::~GpuProfiler() {
    VkDevice dev       = m_device.getDevice();
    for (uint32_t f = 0; f < m_framesInFlight; ++f) {
        if (m_pools[f])
            vkDestroyQueryPool(dev, m_pools[f], nullptr);
    }
    m_readbackBuffers.clear();
}

void GpuProfiler::beginPass(VkCommandBuffer cmd, GpuPass pass, uint32_t frameIndex) {
    if (!m_supported) return;
    uint32_t slot = static_cast<uint32_t>(pass) * 2; // begin slot
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        m_pools[frameIndex], slot);
}

void GpuProfiler::endPass(VkCommandBuffer cmd, GpuPass pass, uint32_t frameIndex) {
    if (!m_supported) return;
    uint32_t slot = static_cast<uint32_t>(pass) * 2 + 1; // end slot
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        m_pools[frameIndex], slot);
}

void GpuProfiler::resolve(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!m_supported) return;
    // Copy query results from pool into readback buffer
    vkCmdCopyQueryPoolResults(
        cmd,
        m_pools[frameIndex],
        0,                             // firstQuery
        STAMPS_PER_FRAME,              // queryCount
        m_readbackBuffers[frameIndex].getBuffer(),    // dstBuffer
        0,                             // dstOffset
        sizeof(uint64_t),              // stride
        VK_QUERY_RESULT_64_BIT         // flags (no WAIT — read after fence)
    );
    m_frameWritten[frameIndex] = true;
}

void GpuProfiler::readback(uint32_t frameIndex) {
    if (!m_supported || !m_frameWritten[frameIndex]) return;

    const uint64_t* ts = static_cast<const uint64_t*>(m_readbackBuffers[frameIndex].map());
    if (!ts) return;

    for (uint32_t p = 0; p < PASS_COUNT; ++p) {
        uint64_t begin = ts[p * 2];
        uint64_t end   = ts[p * 2 + 1];
        if (end >= begin) {
            double ticks = static_cast<double>(end - begin);
            m_results[p] = static_cast<float>(ticks * m_periodNs / 1e6); // ns → ms
        }
    }
}

void GpuProfiler::resetPool(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!m_supported) return;
    vkCmdResetQueryPool(cmd, m_pools[frameIndex], 0, STAMPS_PER_FRAME);
}

float GpuProfiler::getMs(GpuPass pass) const {
    return m_results[static_cast<uint32_t>(pass)];
}

void GpuProfiler::fillSummary(char* buf, size_t bufLen) const {
    if (!m_supported) {
        std::snprintf(buf, bufLen, "GPU profiling: not supported");
        return;
    }
    std::snprintf(buf, bufLen,
                  "Shadow %.2fms  Scene %.2fms  Post %.2fms  ImGui %.2fms",
                  m_results[static_cast<uint32_t>(GpuPass::Shadow)],
                  m_results[static_cast<uint32_t>(GpuPass::Scene)],
                  m_results[static_cast<uint32_t>(GpuPass::Post)],
                  m_results[static_cast<uint32_t>(GpuPass::ImGui)]);
}

} // namespace glory
