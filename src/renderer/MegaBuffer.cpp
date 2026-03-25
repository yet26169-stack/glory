#include "renderer/MegaBuffer.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <stdexcept>

namespace glory {

void MegaBuffer::init(const Device& device, uint32_t vertexCapacity, uint32_t indexCapacity) {
    m_device         = &device;
    m_vertexCapacity = vertexCapacity;
    m_indexCapacity  = indexCapacity;

    VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(vertexCapacity) * sizeof(Vertex);
    VkDeviceSize indexBytes  = static_cast<VkDeviceSize>(indexCapacity)  * sizeof(uint32_t);

    VmaAllocator alloc = device.getAllocator();

    // Device-local mega-buffers (VERTEX + INDEX + TRANSFER_DST)
    m_vertexBuffer = Buffer(alloc, vertexBytes,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VMA_MEMORY_USAGE_GPU_ONLY);

    m_indexBuffer = Buffer(alloc, indexBytes,
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VMA_MEMORY_USAGE_GPU_ONLY);

    // CPU-visible staging buffers
    m_vertexStaging = Buffer(alloc, vertexBytes,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VMA_MEMORY_USAGE_CPU_ONLY);

    m_indexStaging = Buffer(alloc, indexBytes,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VMA_MEMORY_USAGE_CPU_ONLY);

    m_vertexMapped = m_vertexStaging.map();
    m_indexMapped  = m_indexStaging.map();

    spdlog::info("MegaBuffer initialized: vertex {:.1f} MB ({} verts), index {:.1f} MB ({} indices)",
                 vertexBytes / (1024.0 * 1024.0), vertexCapacity,
                 indexBytes / (1024.0 * 1024.0), indexCapacity);
}

void MegaBuffer::destroy() {
    m_vertexStaging.destroy();
    m_indexStaging.destroy();
    m_vertexBuffer.destroy();
    m_indexBuffer.destroy();
    m_handles.clear();
    m_device = nullptr;
}

MeshHandle MegaBuffer::suballocate(const Vertex* vertices, uint32_t vertexCount,
                                    const uint32_t* indices, uint32_t indexCount) {
    if (m_flushed)
        throw std::runtime_error("MegaBuffer: cannot suballocate after flush");

    if (m_nextVertex + vertexCount > m_vertexCapacity)
        throw std::runtime_error("MegaBuffer: vertex capacity exceeded");
    if (m_nextIndex + indexCount > m_indexCapacity)
        throw std::runtime_error("MegaBuffer: index capacity exceeded");

    MeshHandle handle{};
    handle.vertexOffset = m_nextVertex;
    handle.indexOffset  = m_nextIndex;
    handle.indexCount   = indexCount;
    handle.vertexCount  = vertexCount;

    // Copy vertex data to staging
    auto* vDst = static_cast<uint8_t*>(m_vertexMapped) +
                 static_cast<size_t>(m_nextVertex) * sizeof(Vertex);
    std::memcpy(vDst, vertices, static_cast<size_t>(vertexCount) * sizeof(Vertex));

    // Copy index data to staging
    auto* iDst = static_cast<uint8_t*>(m_indexMapped) +
                 static_cast<size_t>(m_nextIndex) * sizeof(uint32_t);
    std::memcpy(iDst, indices, static_cast<size_t>(indexCount) * sizeof(uint32_t));

    m_nextVertex += vertexCount;
    m_nextIndex  += indexCount;

    m_handles.push_back(handle);
    return handle;
}

void MegaBuffer::flush() {
    if (m_flushed) return;
    m_flushed = true;

    m_vertexStaging.flush();
    m_indexStaging.flush();

    VkDeviceSize usedVertexBytes = static_cast<VkDeviceSize>(m_nextVertex) * sizeof(Vertex);
    VkDeviceSize usedIndexBytes  = static_cast<VkDeviceSize>(m_nextIndex)  * sizeof(uint32_t);

    if (usedVertexBytes == 0 && usedIndexBytes == 0) {
        spdlog::info("MegaBuffer: nothing to flush (0 verts, 0 indices)");
        return;
    }

    // One-shot command buffer for the copy — lock pool for thread safety
    auto poolLock = m_device->lockTransferPool();
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool        = m_device->getTransferCommandPool();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(m_device->getDevice(), &allocInfo, &cmd),
             "MegaBuffer: alloc transfer cmd");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    if (usedVertexBytes > 0) {
        VkBufferCopy region{};
        region.size = usedVertexBytes;
        vkCmdCopyBuffer(cmd, m_vertexStaging.getBuffer(),
                        m_vertexBuffer.getBuffer(), 1, &region);
    }
    if (usedIndexBytes > 0) {
        VkBufferCopy region{};
        region.size = usedIndexBytes;
        vkCmdCopyBuffer(cmd, m_indexStaging.getBuffer(),
                        m_indexBuffer.getBuffer(), 1, &region);
    }

    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos    = &cmdInfo;

    m_device->submitTransfer(1, &submitInfo);
    m_device->transferQueueWaitIdle();

    vkFreeCommandBuffers(m_device->getDevice(), m_device->getTransferCommandPool(), 1, &cmd);

    // Free staging memory
    m_vertexStaging.destroy();
    m_indexStaging.destroy();
    m_vertexMapped = nullptr;
    m_indexMapped  = nullptr;

    spdlog::info("MegaBuffer flushed: {} verts ({:.1f} KB), {} indices ({:.1f} KB), {} handles",
                 m_nextVertex, usedVertexBytes / 1024.0,
                 m_nextIndex, usedIndexBytes / 1024.0,
                 m_handles.size());
}

void MegaBuffer::bind(VkCommandBuffer cmd) const {
    VkBuffer     vb  = m_vertexBuffer.getBuffer();
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
}

} // namespace glory
