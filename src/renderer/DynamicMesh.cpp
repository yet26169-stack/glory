#include "renderer/DynamicMesh.h"
#include "renderer/Device.h"

#include <cstring>
#include <spdlog/spdlog.h>

namespace glory {

DynamicMesh::DynamicMesh(const Device &device, VmaAllocator allocator,
                         const std::vector<Vertex> &initialVertices,
                         const std::vector<uint32_t> &indices) {
  m_vertexCount = static_cast<uint32_t>(initialVertices.size());
  m_indexCount = static_cast<uint32_t>(indices.size());

  VkDeviceSize vertBufSize = sizeof(Vertex) * m_vertexCount;

  // Create double-buffered CPU_TO_GPU vertex buffers (persistently mapped)
  m_vertexBuffers.reserve(Sync::MAX_FRAMES_IN_FLIGHT);
  m_mapped.resize(Sync::MAX_FRAMES_IN_FLIGHT);

  for (uint32_t i = 0; i < Sync::MAX_FRAMES_IN_FLIGHT; ++i) {
    m_vertexBuffers.emplace_back(allocator, vertBufSize,
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_mapped[i] = m_vertexBuffers[i].map();
    // Initialize with bind pose
    std::memcpy(m_mapped[i], initialVertices.data(), vertBufSize);
  }

  // Create device-local index buffer (static, never changes)
  m_indexBuffer = Buffer::createDeviceLocal(
      device, allocator, indices.data(),
      sizeof(uint32_t) * m_indexCount,
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

  spdlog::info("DynamicMesh created: {} vertices, {} indices",
               m_vertexCount, m_indexCount);
}

void DynamicMesh::updateVertices(uint32_t frameIndex,
                                 const std::vector<Vertex> &vertices) {
  if (frameIndex >= m_mapped.size() || !m_mapped[frameIndex])
    return;
  size_t copySize = sizeof(Vertex) * std::min(static_cast<uint32_t>(vertices.size()),
                                               m_vertexCount);
  std::memcpy(m_mapped[frameIndex], vertices.data(), copySize);
}

void DynamicMesh::bind(VkCommandBuffer cmd, uint32_t frameIndex) const {
  VkBuffer vertBufs[] = {m_vertexBuffers[frameIndex].getBuffer()};
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(cmd, 0, 1, vertBufs, offsets);
  vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
}

void DynamicMesh::draw(VkCommandBuffer cmd) const {
  vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

} // namespace glory
