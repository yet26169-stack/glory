#pragma once
// ── Async Texture Streamer ────────────────────────────────────────────────
// Loads textures from disk on a background thread and uploads them via the
// dedicated transfer queue so the graphics queue is never stalled.
//
// Two-phase upload (handles dedicated transfer queues lacking blit support):
//   Phase 1 — transfer queue: decode on CPU, copy staging → mip0.
//             Image created with VK_SHARING_MODE_CONCURRENT when families differ,
//             so no explicit ownership release/acquire is needed.
//   Phase 2 — graphics queue: blit mip chain (requires GRAPHICS capability),
//             transition all mips to SHADER_READ_ONLY_OPTIMAL.
//             Submitted from tick() once xferFence signals.
//   Phase 3 — main thread tick(): gfxFence polls done; fires ready callback.
//
// The ready callback receives (slot, imageView, sampler) — caller owns the
// image view and sampler lifetime from that point.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "renderer/Buffer.h"

namespace glory {

class Device;

// Callback: called on the main thread once a texture is fully ready to bind.
// (slot, imageView, sampler) — caller takes ownership of view+sampler.
using StreamReadyCallback = std::function<void(uint32_t, VkImageView, VkSampler)>;

struct StreamEntry {
    std::string   path;
    uint32_t      slot        = 0;
    Buffer        staging;
    VkImage       image       = VK_NULL_HANDLE;
    VmaAllocation imageAlloc  = VK_NULL_HANDLE;
    VkImageView   view        = VK_NULL_HANDLE;
    VkSampler     sampler     = VK_NULL_HANDLE;
    uint32_t      width       = 0;
    uint32_t      height      = 0;
    uint32_t      mipLevels   = 1;
    // Phase 1: transfer queue copies staging → mip0
    VkFence       xferFence   = VK_NULL_HANDLE;
    // Phase 2: graphics queue blits mip chain
    VkFence       gfxFence    = VK_NULL_HANDLE;
    bool          ready       = false;
};

class TextureStreamer {
public:
    TextureStreamer() = default;
    ~TextureStreamer() { destroy(); }

    TextureStreamer(const TextureStreamer&)            = delete;
    TextureStreamer& operator=(const TextureStreamer&) = delete;

    void init(const Device& device, StreamReadyCallback onReady);
    void destroy();

    // Enqueue an async load. Returns immediately. slot → ready callback.
    void requestLoad(const std::string& path, uint32_t slot);

    // Call every frame on the main thread. Polls fences, fires ready callbacks.
    void tick();

    uint32_t pendingCount() const {
        return static_cast<uint32_t>(m_pending.load(std::memory_order_relaxed));
    }

private:
    struct LoadRequest { std::string path; uint32_t slot; };

    void            workerLoop();
    StreamEntry     processRequest(const LoadRequest& req);
    void            submitMipGen(StreamEntry& e);
    VkCommandBuffer beginOneTimeCmd(VkCommandPool pool);
    VkSampler       createSampler(uint32_t mipLevels, float mipBias);

    const Device*        m_device   = nullptr;
    VkDevice             m_vkDevice = VK_NULL_HANDLE;
    StreamReadyCallback  m_onReady;

    std::thread          m_worker;
    std::atomic<bool>    m_stop{false};
    std::atomic<int>     m_pending{0};

    std::mutex               m_workMutex;
    std::condition_variable  m_workCV;
    std::queue<LoadRequest>  m_workQueue;

    std::mutex               m_doneMutex;
    std::vector<StreamEntry> m_doneEntries;
};

} // namespace glory
