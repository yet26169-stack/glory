#pragma once

#include "renderer/Buffer.h"
#include "renderer/Device.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace glory {

// Handle returned by MegaBuffer::suballocate().
// All offsets are in elements (vertices / indices), not bytes.
struct MeshHandle {
    uint32_t vertexOffset = 0;  // first vertex in mega vertex buffer
    uint32_t indexOffset  = 0;  // first index in mega index buffer
    uint32_t indexCount   = 0;
    uint32_t vertexCount  = 0;
};

// A single pair of large VkBuffers (vertex + index) into which all static
// mesh geometry is sub-allocated.  Eliminates per-mesh buffer creation and
// allows the GPU-driven indirect draw path to bind once per frame.
//
// Usage:
//   1. Construct with capacities
//   2. Call suballocate() for each mesh (writes to CPU staging)
//   3. Call flush() once to copy staging → device-local
//   4. Call bind() each frame before indirect draws
class MegaBuffer {
public:
    MegaBuffer() = default;

    // vertexCapacity / indexCapacity are in ELEMENTS, not bytes.
    void init(const Device& device, uint32_t vertexCapacity, uint32_t indexCapacity);
    void destroy();

    // Copy vertex + index data into the staging area and return a handle
    // describing where the data was placed.  Must be called BEFORE flush().
    MeshHandle suballocate(const Vertex* vertices, uint32_t vertexCount,
                           const uint32_t* indices, uint32_t indexCount);

    // Transfer staging → device-local.  Call once after all suballocate() calls.
    void flush();

    // Bind the mega vertex buffer (slot 0) and mega index buffer.
    void bind(VkCommandBuffer cmd) const;

    VkBuffer getVertexBuffer() const { return m_vertexBuffer.getBuffer(); }
    VkBuffer getIndexBuffer()  const { return m_indexBuffer.getBuffer(); }

    uint32_t getVertexCount()  const { return m_nextVertex; }
    uint32_t getIndexCount()   const { return m_nextIndex; }

    const std::vector<MeshHandle>& getHandles() const { return m_handles; }

private:
    const Device* m_device = nullptr;

    // Device-local mega-buffers
    Buffer m_vertexBuffer;
    Buffer m_indexBuffer;

    // CPU-visible staging buffers (destroyed after flush)
    Buffer m_vertexStaging;
    Buffer m_indexStaging;

    uint32_t m_vertexCapacity = 0;
    uint32_t m_indexCapacity  = 0;
    uint32_t m_nextVertex     = 0;
    uint32_t m_nextIndex      = 0;

    void* m_vertexMapped = nullptr;
    void* m_indexMapped  = nullptr;
    bool  m_flushed      = false;

    // Every handle ever allocated (indexed by order of suballocate calls)
    std::vector<MeshHandle> m_handles;
};

} // namespace glory
