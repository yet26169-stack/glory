#include "renderer/StaticSkinnedMesh.h"
#include "renderer/Device.h"

#include <spdlog/spdlog.h>

namespace glory {

StaticSkinnedMesh::StaticSkinnedMesh(const Device& device, VmaAllocator allocator,
                                     const std::vector<SkinnedVertex>& vertices,
                                     const std::vector<uint32_t>& indices)
{
    m_vertexCount = static_cast<uint32_t>(vertices.size());
    m_indexCount  = static_cast<uint32_t>(indices.size());

    m_vertexBuffer = Buffer::createDeviceLocal(
        device, allocator,
        vertices.data(), sizeof(SkinnedVertex) * m_vertexCount,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    m_indexBuffer = Buffer::createDeviceLocal(
        device, allocator,
        indices.data(), sizeof(uint32_t) * m_indexCount,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    spdlog::info("StaticSkinnedMesh created: {} vertices, {} indices "
                 "({:.1f} KB vertex, {:.1f} KB index)",
                 m_vertexCount, m_indexCount,
                 sizeof(SkinnedVertex) * m_vertexCount / 1024.0f,
                 sizeof(uint32_t) * m_indexCount / 1024.0f);
}

void StaticSkinnedMesh::bind(VkCommandBuffer cmd) const {
    VkBuffer     buf     = m_vertexBuffer.getBuffer();
    VkDeviceSize offset  = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
}

void StaticSkinnedMesh::draw(VkCommandBuffer cmd) const {
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

} // namespace glory
