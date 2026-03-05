#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace glory {

class Device;

// ── Vertex format ───────────────────────────────────────────────────────────
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
    glm::vec3 normal;
    glm::vec2 texCoord;

    bool operator==(const Vertex& other) const {
        return position == other.position && color == other.color &&
               normal == other.normal && texCoord == other.texCoord;
    }

    static VkVertexInputBindingDescription getBindingDescription();
    static std::array<VkVertexInputAttributeDescription, 4> getAttributeDescriptions();
};

// ── Skinned vertex: Vertex fields + bone joints/weights ─────────────────────
// Used as binding 0 in the GPU-skinned pipeline (static, never changes per frame)
struct SkinnedVertex {
    glm::vec3 position;   // location 0
    glm::vec3 color;      // location 1
    glm::vec3 normal;     // location 2
    glm::vec2 texCoord;   // location 3
    glm::ivec4 joints;    // location 4 — bone indices (up to 4)
    glm::vec4  weights;   // location 5 — bone weights (sum to 1.0)

    static VkVertexInputBindingDescription getBindingDescription();
    static std::array<VkVertexInputAttributeDescription, 6> getAttributeDescriptions();
};

// ── Per-instance data for instanced rendering ───────────────────────────────
struct InstanceData {
    glm::mat4 model;        // locations 4,5,6,7
    glm::mat4 normalMatrix; // locations 8,9,10,11
    glm::vec4 tint;         // location 12
    glm::vec4 params;       // location 13 (shininess, metallic, roughness, emissive)
    glm::vec4 texIndices;   // location 14 (x=diffuseIdx, y=normalIdx, z/w=reserved)

    static VkVertexInputBindingDescription getBindingDescription();
    static std::array<VkVertexInputAttributeDescription, 11> getAttributeDescriptions();
};

// ── RAII GPU buffer ─────────────────────────────────────────────────────────
class Buffer {
public:
    Buffer() = default;
    Buffer(VmaAllocator allocator, VkDeviceSize size,
           VkBufferUsageFlags usage, VmaMemoryUsage memUsage);
    ~Buffer();

    // Move-only (Rule of 5)
    Buffer(const Buffer&)            = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    void* map();
    void  unmap();

    VkBuffer      getBuffer()    const { return m_buffer; }
    VmaAllocation getAllocation() const { return m_allocation; }
    VkDeviceSize  getSize()      const { return m_size; }

    // Utility: create a device-local buffer filled via a staging copy.
    static Buffer createDeviceLocal(
        const Device& device, VmaAllocator allocator,
        const void* data, VkDeviceSize size,
        VkBufferUsageFlags usage);

private:
    VmaAllocator  m_allocator  = VK_NULL_HANDLE;
    VkBuffer      m_buffer     = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkDeviceSize  m_size       = 0;
    void*         m_mappedData = nullptr;
    bool          m_manuallyMapped = false;

public:
    void destroy();
};

} // namespace glory
