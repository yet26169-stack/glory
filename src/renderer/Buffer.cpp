#include "renderer/Buffer.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace glory {

// ── Vertex ──────────────────────────────────────────────────────────────────
VkVertexInputBindingDescription Vertex::getBindingDescription() {
    VkVertexInputBindingDescription desc{};
    desc.binding   = 0;
    desc.stride    = sizeof(Vertex);
    desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return desc;
}

std::array<VkVertexInputAttributeDescription, 4> Vertex::getAttributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 4> attrs{};
    // location 0: position
    attrs[0].binding  = 0;
    attrs[0].location = 0;
    attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset   = offsetof(Vertex, position);
    // location 1: color
    attrs[1].binding  = 0;
    attrs[1].location = 1;
    attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset   = offsetof(Vertex, color);
    // location 2: normal
    attrs[2].binding  = 0;
    attrs[2].location = 2;
    attrs[2].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[2].offset   = offsetof(Vertex, normal);
    // location 3: texCoord
    attrs[3].binding  = 0;
    attrs[3].location = 3;
    attrs[3].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[3].offset   = offsetof(Vertex, texCoord);
    return attrs;
}

// ── InstanceData ────────────────────────────────────────────────────────────
VkVertexInputBindingDescription InstanceData::getBindingDescription() {
    VkVertexInputBindingDescription desc{};
    desc.binding   = 1;
    desc.stride    = sizeof(InstanceData);
    desc.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    return desc;
}

std::array<VkVertexInputAttributeDescription, 11> InstanceData::getAttributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 11> attrs{};
    // model matrix: 4 vec4 columns at locations 4-7
    for (uint32_t i = 0; i < 4; ++i) {
        attrs[i].binding  = 1;
        attrs[i].location = 4 + i;
        attrs[i].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[i].offset   = offsetof(InstanceData, model) + sizeof(glm::vec4) * i;
    }
    // normalMatrix: 4 vec4 columns at locations 8-11
    for (uint32_t i = 0; i < 4; ++i) {
        attrs[4 + i].binding  = 1;
        attrs[4 + i].location = 8 + i;
        attrs[4 + i].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[4 + i].offset   = offsetof(InstanceData, normalMatrix) + sizeof(glm::vec4) * i;
    }
    // tint: vec4 at location 12
    attrs[8].binding  = 1;
    attrs[8].location = 12;
    attrs[8].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[8].offset   = offsetof(InstanceData, tint);
    // params: vec4 at location 13 (shininess, metallic, roughness, emissive)
    attrs[9].binding  = 1;
    attrs[9].location = 13;
    attrs[9].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[9].offset   = offsetof(InstanceData, params);
    // texIndices: vec4 at location 14 (x=diffuseIdx, y=normalIdx)
    attrs[10].binding  = 1;
    attrs[10].location = 14;
    attrs[10].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[10].offset   = offsetof(InstanceData, texIndices);
    return attrs;
}

// ── Buffer lifecycle ────────────────────────────────────────────────────────
Buffer::Buffer(VmaAllocator allocator, VkDeviceSize size,
               VkBufferUsageFlags usage, VmaMemoryUsage memUsage)
    : m_allocator(allocator), m_size(size)
{
    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size  = size;
    bufCI.usage = usage;
    bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = memUsage;

    VK_CHECK(vmaCreateBuffer(m_allocator, &bufCI, &allocCI,
                             &m_buffer, &m_allocation, nullptr),
             "Failed to create VMA buffer");
}

Buffer::~Buffer() { destroy(); }

Buffer::Buffer(Buffer&& other) noexcept
    : m_allocator(other.m_allocator)
    , m_buffer(other.m_buffer)
    , m_allocation(other.m_allocation)
    , m_size(other.m_size)
{
    other.m_allocator  = VK_NULL_HANDLE;
    other.m_buffer     = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_size       = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        m_allocator  = other.m_allocator;
        m_buffer     = other.m_buffer;
        m_allocation = other.m_allocation;
        m_size       = other.m_size;
        other.m_allocator  = VK_NULL_HANDLE;
        other.m_buffer     = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
        other.m_size       = 0;
    }
    return *this;
}

void* Buffer::map() {
    void* data;
    VK_CHECK(vmaMapMemory(m_allocator, m_allocation, &data),
             "Failed to map buffer memory");
    return data;
}

void Buffer::unmap() {
    vmaUnmapMemory(m_allocator, m_allocation);
}

void Buffer::destroy() {
    if (m_buffer != VK_NULL_HANDLE && m_allocator != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
        m_buffer     = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
    }
}

// ── Staging upload utility ──────────────────────────────────────────────────
Buffer Buffer::createDeviceLocal(
    const Device& device, VmaAllocator allocator,
    const void* data, VkDeviceSize size,
    VkBufferUsageFlags usage)
{
    // 1. Create host-visible staging buffer
    Buffer staging(allocator, size,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VMA_MEMORY_USAGE_CPU_ONLY);
    void* mapped = staging.map();
    std::memcpy(mapped, data, static_cast<size_t>(size));
    staging.unmap();

    // 2. Create device-local target buffer
    Buffer target(allocator, size,
                  usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VMA_MEMORY_USAGE_GPU_ONLY);

    // 3. One-shot command buffer for the copy
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool        = device.getTransferCommandPool();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device.getDevice(), &allocInfo, &cmd),
             "Failed to allocate staging command buffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(cmd, staging.getBuffer(), target.getBuffer(), 1, &copyRegion);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;

    vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(device.getGraphicsQueue());

    vkFreeCommandBuffers(device.getDevice(), device.getTransferCommandPool(), 1, &cmd);

    return target;
}

} // namespace glory
