#include "renderer/Mesh.h"
#include "renderer/Device.h"

namespace glory {

Mesh::Mesh(const Device& device, VmaAllocator allocator,
           const std::vector<Vertex>& vertices,
           const std::vector<uint32_t>& indices)
    : m_indexCount(static_cast<uint32_t>(indices.size()))
    , m_cpuVertices(vertices)
    , m_cpuIndices(indices)
{
    m_vertexBuffer = Buffer::createDeviceLocal(
        device, allocator,
        vertices.data(), sizeof(Vertex) * vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    m_indexBuffer = Buffer::createDeviceLocal(
        device, allocator,
        indices.data(), sizeof(uint32_t) * indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void Mesh::bind(VkCommandBuffer cmd) const {
    VkBuffer     buffers[] = { m_vertexBuffer.getBuffer() };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
}

void Mesh::draw(VkCommandBuffer cmd) const {
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

void Mesh::drawInstanced(VkCommandBuffer cmd, uint32_t instanceCount, uint32_t firstInstance) const {
    vkCmdDrawIndexed(cmd, m_indexCount, instanceCount, 0, 0, firstInstance);
}

void Mesh::drawIndirect(VkCommandBuffer cmd, VkBuffer indirectBuffer, VkDeviceSize offset) const {
    vkCmdDrawIndexedIndirect(cmd, indirectBuffer, offset, 1,
                             sizeof(VkDrawIndexedIndirectCommand));
}

} // namespace glory
