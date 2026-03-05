#pragma once
// GpuProfiler.h — per-pass GPU timestamp queries

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>
#include "renderer/Buffer.h"

namespace glory {

class Device;

// ── Pass enum ───────────────────────────────────────────────────────────────
enum class GpuPass : uint32_t {
    Shadow  = 0,
    Scene   = 1,
    Post    = 2,
    ImGui   = 3,
    COUNT   = 4
};

inline std::string_view gpuPassName(GpuPass p) {
    switch (p) {
    case GpuPass::Shadow: return "Shadow";
    case GpuPass::Scene:  return "Scene";
    case GpuPass::Post:   return "Post";
    case GpuPass::ImGui:  return "ImGui";
    default:              return "Unknown";
    }
}

// ── GpuProfiler ─────────────────────────────────────────────────────────────
class GpuProfiler {
public:
    // frames_in_flight: how many frames we double-buffer (matches Sync::MAX_FRAMES_IN_FLIGHT)
    GpuProfiler(const Device& device, uint32_t framesInFlight);
    ~GpuProfiler();

    GpuProfiler(const GpuProfiler&)            = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    // Call BEFORE the pass begins (outside vkCmdBeginRenderPass for shadow/post;
    // can be inside for scene draws)
    void beginPass(VkCommandBuffer cmd, GpuPass pass, uint32_t frameIndex);

    // Call AFTER the pass ends
    void endPass(VkCommandBuffer cmd, GpuPass pass, uint32_t frameIndex);

    // Call once at the end of the command buffer (after all passes).
    // Copies the pool results into a host-visible readback buffer.
    void resolve(VkCommandBuffer cmd, uint32_t frameIndex);

    // Call at the START of the command buffer each frame to reset all query slots.
    void resetPool(VkCommandBuffer cmd, uint32_t frameIndex);

    // Call AFTER the frame fence has been signaled (safe to read GPU results).
    // Populates the internal timing table.
    void readback(uint32_t frameIndex);

    // Returns the most recently read GPU time in milliseconds for the given pass.
    float getMs(GpuPass pass) const;

    // Returns a summary string for all passes, e.g. for ImGui overlay.
    // Format: "Shadow 0.3ms  Scene 2.1ms  Post 0.4ms  ImGui 0.1ms"
    void fillSummary(char* buf, size_t bufLen) const;

    bool isSupported() const { return m_supported; }

private:
    static constexpr uint32_t PASS_COUNT  = static_cast<uint32_t>(GpuPass::COUNT);
    static constexpr uint32_t STAMPS_PER_FRAME = PASS_COUNT * 2; // begin + end per pass

    const Device& m_device;
    uint32_t      m_framesInFlight = 0;
    bool          m_supported      = false;
    float         m_periodNs       = 1.0f; // GPU timestamp period in nanoseconds

    // One query pool per frame-in-flight
    std::vector<VkQueryPool>  m_pools;
    // Readback buffers: host-visible, one per frame (persistently mapped via Buffer)
    std::vector<Buffer>       m_readbackBuffers;
    
    // Tells us which queries have been written this frame
    std::vector<bool>         m_frameWritten;

    // Latest timing results in ms (indexed by GpuPass)
    std::array<float, PASS_COUNT> m_results{};
};

} // namespace glory
