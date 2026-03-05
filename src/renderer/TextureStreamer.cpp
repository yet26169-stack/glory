#include "renderer/TextureStreamer.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

// stb_image is already defined in Texture.cpp — just declare the API here
#include <stb_image.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace glory {

// ── Initialise / destroy ──────────────────────────────────────────────────
void TextureStreamer::init(const Device& device, StreamReadyCallback onReady) {
    m_device   = &device;
    m_vkDevice = device.getDevice();
    m_onReady  = std::move(onReady);
    m_stop     = false;
    m_worker   = std::thread([this]{ workerLoop(); });
    spdlog::info("TextureStreamer: background upload thread started");
}

void TextureStreamer::destroy() {
    if (m_worker.joinable()) {
        {
            std::lock_guard<std::mutex> lk(m_workMutex);
            m_stop = true;
        }
        m_workCV.notify_one();
        m_worker.join();
    }

    // Worker is stopped — safe to access m_doneEntries without lock
    for (auto& e : m_doneEntries) {
        if (e.xferFence)   { vkWaitForFences(m_vkDevice, 1, &e.xferFence, VK_TRUE, UINT64_MAX); vkDestroyFence(m_vkDevice, e.xferFence,  nullptr); }
        if (e.gfxFence)    { vkWaitForFences(m_vkDevice, 1, &e.gfxFence,  VK_TRUE, UINT64_MAX); vkDestroyFence(m_vkDevice, e.gfxFence,   nullptr); }
        e.staging.destroy();
        if (e.view)        vkDestroyImageView(m_vkDevice, e.view, nullptr);
        if (e.sampler)     vkDestroySampler(m_vkDevice, e.sampler, nullptr);
        if (e.image)       vmaDestroyImage(m_device->getAllocator(), e.image, e.imageAlloc);
    }
    m_doneEntries.clear();
    m_vkDevice = VK_NULL_HANDLE;
    m_device   = nullptr;
}

// ── Enqueue request ───────────────────────────────────────────────────────
void TextureStreamer::requestLoad(const std::string& path, uint32_t slot) {
    {
        std::lock_guard<std::mutex> lk(m_workMutex);
        m_workQueue.push({path, slot});
    }
    m_pending.fetch_add(1, std::memory_order_relaxed);
    m_workCV.notify_one();
    spdlog::debug("TextureStreamer: queued '{}' → slot {}", path, slot);
}

// ── Worker loop ───────────────────────────────────────────────────────────
void TextureStreamer::workerLoop() {
    while (true) {
        LoadRequest req;
        {
            std::unique_lock<std::mutex> lk(m_workMutex);
            m_workCV.wait(lk, [this]{
                return m_stop.load() || !m_workQueue.empty();
            });
            if (m_stop && m_workQueue.empty()) break;
            req = std::move(m_workQueue.front());
            m_workQueue.pop();
        }

        try {
            StreamEntry entry = processRequest(req);
            {
                std::lock_guard<std::mutex> lk(m_doneMutex);
                m_doneEntries.push_back(std::move(entry));
            }
        } catch (const std::exception& e) {
            spdlog::error("TextureStreamer: failed to load '{}': {}", req.path, e.what());
            m_pending.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

// ── Load + upload one texture ─────────────────────────────────────────────
StreamEntry TextureStreamer::processRequest(const LoadRequest& req) {
    StreamEntry entry;
    entry.slot = req.slot;
    entry.path = req.path;

    auto qf = m_device->getQueueFamilies();
    uint32_t xferFamily = qf.transferFamily.value();
    uint32_t gfxFamily  = qf.graphicsFamily.value();
    bool dedicatedXfer  = (xferFamily != gfxFamily);

    // Decode image on CPU
    int w, h, channels;
    stbi_uc* pixels = stbi_load(req.path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels)
        throw std::runtime_error(std::string("stbi_load failed: ") + stbi_failure_reason());

    entry.width  = static_cast<uint32_t>(w);
    entry.height = static_cast<uint32_t>(h);
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(w * h * 4);
    entry.mipLevels = static_cast<uint32_t>(
        std::floor(std::log2(std::max(w, h)))) + 1;

    // Create staging buffer (persistently mapped via Buffer class)
    entry.staging = Buffer(m_device->getAllocator(), imageSize,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VMA_MEMORY_USAGE_CPU_ONLY);
    std::memcpy(entry.staging.map(), pixels, imageSize);
    stbi_image_free(pixels);

    // Create GPU image
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType   = VK_IMAGE_TYPE_2D;
        ici.format      = VK_FORMAT_R8G8B8A8_SRGB;
        ici.extent      = {entry.width, entry.height, 1};
        ici.mipLevels   = entry.mipLevels;
        ici.arrayLayers = 1;
        ici.samples     = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling      = VK_IMAGE_TILING_OPTIMAL;
        ici.usage       = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        uint32_t familyIndices[2] = {xferFamily, gfxFamily};
        if (dedicatedXfer) {
            ici.sharingMode           = VK_SHARING_MODE_CONCURRENT;
            ici.queueFamilyIndexCount = 2;
            ici.pQueueFamilyIndices   = familyIndices;
        }
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        VK_CHECK(vmaCreateImage(m_device->getAllocator(), &ici, &aci,
                                &entry.image, &entry.imageAlloc, nullptr),
                 "TextureStreamer image");
    }

    // ── Phase 1: transfer queue — copy staging → mip0 ────────────────────
    {
        VkCommandBuffer cmd = beginOneTimeCmd(m_device->getTransferCommandPool());

        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = entry.image;
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, entry.mipLevels, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {entry.width, entry.height, 1};
        vkCmdCopyBufferToImage(cmd, entry.staging.getBuffer(), entry.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(m_vkDevice, &fci, nullptr, &entry.xferFence),
                 "TextureStreamer xfer fence");

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cmd;
        VK_CHECK(vkQueueSubmit(m_device->getTransferQueue(), 1, &si, entry.xferFence),
                 "TextureStreamer xfer submit");
        vkFreeCommandBuffers(m_vkDevice, m_device->getTransferCommandPool(), 1, &cmd);
    }

    {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image    = entry.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = VK_FORMAT_R8G8B8A8_SRGB;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, entry.mipLevels, 0, 1};
        VK_CHECK(vkCreateImageView(m_vkDevice, &vci, nullptr, &entry.view),
                 "TextureStreamer image view");
        entry.sampler = createSampler(entry.mipLevels, 0.0f);
    }

    entry.ready = false;
    return entry;
}

void TextureStreamer::submitMipGen(StreamEntry& e) {
    VkCommandBuffer cmd = beginOneTimeCmd(m_device->getGraphicsCommandPool());

    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.image               = e.image;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.baseArrayLayer = 0;
    bar.subresourceRange.layerCount     = 1;
    bar.subresourceRange.levelCount     = 1;

    int32_t mipW = static_cast<int32_t>(e.width);
    int32_t mipH = static_cast<int32_t>(e.height);

    for (uint32_t i = 1; i < e.mipLevels; ++i) {
        bar.subresourceRange.baseMipLevel = i - 1;
        bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);

        int32_t nextW = std::max(mipW / 2, 1);
        int32_t nextH = std::max(mipH / 2, 1);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i-1, 0, 1};
        blit.srcOffsets[0]  = {0, 0, 0};
        blit.srcOffsets[1]  = {mipW, mipH, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        blit.dstOffsets[0]  = {0, 0, 0};
        blit.dstOffsets[1]  = {nextW, nextH, 1};
        vkCmdBlitImage(cmd,
            e.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);

        mipW = nextW;
        mipH = nextH;
    }

    bar.subresourceRange.baseMipLevel = e.mipLevels - 1;
    bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &bar);

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(m_vkDevice, &fci, nullptr, &e.gfxFence),
             "TextureStreamer gfx fence");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    VK_CHECK(vkQueueSubmit(m_device->getGraphicsQueue(), 1, &si, e.gfxFence),
             "TextureStreamer gfx submit");
    vkFreeCommandBuffers(m_vkDevice, m_device->getGraphicsCommandPool(), 1, &cmd);
}

void TextureStreamer::tick() {
    std::vector<StreamEntry> done;
    {
        std::lock_guard<std::mutex> lk(m_doneMutex);
        done = std::move(m_doneEntries);
        m_doneEntries.clear();
    }

    std::vector<StreamEntry> stillPending;
    for (auto& e : done) {
        if (e.gfxFence != VK_NULL_HANDLE) {
            if (vkGetFenceStatus(m_vkDevice, e.gfxFence) == VK_SUCCESS) {
                vkDestroyFence(m_vkDevice, e.gfxFence, nullptr);
                e.gfxFence = VK_NULL_HANDLE;
                e.staging.destroy();
                m_pending.fetch_sub(1, std::memory_order_relaxed);
                spdlog::debug("TextureStreamer: '{}' ready → slot {}", e.path, e.slot);
                if (m_onReady) m_onReady(e.slot, e.view, e.sampler);
            } else {
                stillPending.push_back(std::move(e));
            }
        } else if (e.xferFence != VK_NULL_HANDLE) {
            if (vkGetFenceStatus(m_vkDevice, e.xferFence) == VK_SUCCESS) {
                vkDestroyFence(m_vkDevice, e.xferFence, nullptr);
                e.xferFence = VK_NULL_HANDLE;
                submitMipGen(e);
                stillPending.push_back(std::move(e));
            } else {
                stillPending.push_back(std::move(e));
            }
        }
    }

    if (!stillPending.empty()) {
        std::lock_guard<std::mutex> lk(m_doneMutex);
        for (auto& e : stillPending)
            m_doneEntries.push_back(std::move(e));
    }
}

VkCommandBuffer TextureStreamer::beginOneTimeCmd(VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(m_vkDevice, &ai, &cmd),
             "TextureStreamer cmd alloc");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

VkSampler TextureStreamer::createSampler(uint32_t mipLevels, float mipBias) {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter        = VK_FILTER_LINEAR;
    sci.minFilter        = VK_FILTER_LINEAR;
    sci.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.mipLodBias       = mipBias;
    sci.anisotropyEnable = VK_TRUE;
    sci.maxAnisotropy    = 4.0f;
    sci.minLod           = 0.0f;
    sci.maxLod           = static_cast<float>(mipLevels);
    VkSampler s;
    VK_CHECK(vkCreateSampler(m_vkDevice, &sci, nullptr, &s),
             "TextureStreamer sampler");
    return s;
}

} // namespace glory
