#pragma once

#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>

namespace glory {

class Device;

// A mesh whose vertices are NEVER updated on the CPU — bind-pose geometry +
// joint/weight data stay in device-local GPU memory.  The vertex shader reads
// a per-frame bone-matrix SSBO (binding 4) and skins on the GPU.
class StaticSkinnedMesh {
public:
    StaticSkinnedMesh() = default;
    StaticSkinnedMesh(const Device& device, VmaAllocator allocator,
                      const std::vector<SkinnedVertex>& vertices,
                      const std::vector<uint32_t>& indices);

    StaticSkinnedMesh(const StaticSkinnedMesh&)            = delete;
    StaticSkinnedMesh& operator=(const StaticSkinnedMesh&) = delete;
    StaticSkinnedMesh(StaticSkinnedMesh&&) noexcept        = default;
    StaticSkinnedMesh& operator=(StaticSkinnedMesh&&) noexcept = default;

    // Bind vertex + index buffers, then draw
    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    uint32_t getIndexCount()  const { return m_indexCount; }
    uint32_t getVertexCount() const { return m_vertexCount; }
    // Returns the device-local bind-pose vertex buffer (used by compute skinning)
    VkBuffer getVertexBuffer() const { return m_vertexBuffer.getBuffer(); }

private:
    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    uint32_t m_indexCount  = 0;
    uint32_t m_vertexCount = 0;
};

} // namespace glory
