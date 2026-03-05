#pragma once

#include "renderer/Buffer.h"
#include "renderer/Sync.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <vector>

namespace glory {

class Device;

// A mesh whose vertex data is updated on the CPU each frame (for CPU skinning).
// Uses double-buffered CPU_TO_GPU vertex buffers (one per frame in flight)
// and a static device-local index buffer.
class DynamicMesh {
public:
  DynamicMesh() = default;
  DynamicMesh(const Device &device, VmaAllocator allocator,
              const std::vector<Vertex> &initialVertices,
              const std::vector<uint32_t> &indices);

  DynamicMesh(const DynamicMesh &) = delete;
  DynamicMesh &operator=(const DynamicMesh &) = delete;
  DynamicMesh(DynamicMesh &&) noexcept = default;
  DynamicMesh &operator=(DynamicMesh &&) noexcept = default;

  // Upload new vertex data for the given frame index
  void updateVertices(uint32_t frameIndex, const std::vector<Vertex> &vertices);

  // Bind vertex + index buffers for the given frame, then draw
  void bind(VkCommandBuffer cmd, uint32_t frameIndex) const;
  void draw(VkCommandBuffer cmd) const;

  uint32_t getIndexCount() const { return m_indexCount; }
  uint32_t getVertexCount() const { return m_vertexCount; }

private:
  std::vector<Buffer> m_vertexBuffers; // one per frame in flight
  std::vector<void *> m_mapped;        // persistently mapped pointers
  Buffer m_indexBuffer;                // device-local, static
  uint32_t m_indexCount = 0;
  uint32_t m_vertexCount = 0;
};

} // namespace glory
