#pragma once

#include "renderer/Buffer.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

namespace glory {

class Device;

class Mesh {
public:
    Mesh() = default;
    Mesh(const Device& device, VmaAllocator allocator,
         const std::vector<Vertex>& vertices,
         const std::vector<uint32_t>& indices);

    Mesh(const Mesh&)            = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) noexcept            = default;
    Mesh& operator=(Mesh&&) noexcept = default;

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;
    void drawInstanced(VkCommandBuffer cmd, uint32_t instanceCount, uint32_t firstInstance) const;
    void drawIndirect(VkCommandBuffer cmd, VkBuffer indirectBuffer, VkDeviceSize offset) const;

    uint32_t getIndexCount() const { return m_indexCount; }

private:
    Buffer   m_vertexBuffer;
    Buffer   m_indexBuffer;
    uint32_t m_indexCount = 0;
};

} // namespace glory
