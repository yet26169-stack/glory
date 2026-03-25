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

// ── SkinnedVertex ────────────────────────────────────────────────────────────
VkVertexInputBindingDescription SkinnedVertex::getBindingDescription() {
    VkVertexInputBindingDescription desc{};
    desc.binding   = 0;
    desc.stride    = sizeof(SkinnedVertex);
    desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return desc;
}

std::array<VkVertexInputAttributeDescription, 6> SkinnedVertex::getAttributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 6> attrs{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(SkinnedVertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(SkinnedVertex, color)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(SkinnedVertex, normal)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT,        offsetof(SkinnedVertex, texCoord)};
    attrs[4] = {4, 0, VK_FORMAT_R32G32B32A32_SINT,    offsetof(SkinnedVertex, joints)};
    attrs[5] = {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT,  offsetof(SkinnedVertex, weights)};
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
    allocCI.usage = VMA_MEMORY_USAGE_AUTO;
    
    if (memUsage == VMA_MEMORY_USAGE_CPU_ONLY || 
        memUsage == VMA_MEMORY_USAGE_CPU_TO_GPU) 
    {
        allocCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | 
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    else if (memUsage == VMA_MEMORY_USAGE_GPU_TO_CPU)
    {
        allocCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | 
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateBuffer(m_allocator, &bufCI, &allocCI,
                             &m_buffer, &m_allocation, &allocInfo),
             "Failed to create VMA buffer");
             
    m_mappedData = allocInfo.pMappedData;
}

Buffer::~Buffer() { destroy(); }

Buffer::Buffer(Buffer&& other) noexcept
    : m_allocator(other.m_allocator)
    , m_buffer(other.m_buffer)
    , m_allocation(other.m_allocation)
    , m_size(other.m_size)
    , m_mappedData(other.m_mappedData)
    , m_manuallyMapped(other.m_manuallyMapped)
{
    other.m_allocator  = VK_NULL_HANDLE;
    other.m_buffer     = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_size       = 0;
    other.m_mappedData = nullptr;
    other.m_manuallyMapped = false;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        m_allocator  = other.m_allocator;
        m_buffer     = other.m_buffer;
        m_allocation = other.m_allocation;
        m_size       = other.m_size;
        m_mappedData = other.m_mappedData;
        m_manuallyMapped = other.m_manuallyMapped;
        other.m_allocator  = VK_NULL_HANDLE;
        other.m_buffer     = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
        other.m_size       = 0;
        other.m_mappedData = nullptr;
        other.m_manuallyMapped = false;
    }
    return *this;
}

void* Buffer::map() {
    if (m_mappedData) return m_mappedData;
    // If not persistently mapped, map it now
    VK_CHECK(vmaMapMemory(m_allocator, m_allocation, &m_mappedData),
             "Failed to map VMA buffer memory");
    m_manuallyMapped = true;
    return m_mappedData;
}

void Buffer::unmap() {
    if (m_manuallyMapped && m_mappedData) {
        vmaUnmapMemory(m_allocator, m_allocation);
        m_mappedData = nullptr;
        m_manuallyMapped = false;
    }
}

void Buffer::flush() {
    if (m_allocation != VK_NULL_HANDLE)
        vmaFlushAllocation(m_allocator, m_allocation, 0, VK_WHOLE_SIZE);
}

void Buffer::destroy() {
    if (m_buffer != VK_NULL_HANDLE && m_allocator != VK_NULL_HANDLE) {
        unmap();
        vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
        m_buffer     = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
        m_mappedData = nullptr;
        m_manuallyMapped = false;
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
    staging.flush();

    // 2. Create device-local target buffer.  Use CONCURRENT sharing when a
    //    dedicated transfer queue is in use, matching the Image.cpp approach.
    Buffer target;
    target.m_allocator = allocator;
    target.m_size      = size;

    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size  = size;
    bufCI.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    uint32_t familyIndices[] = {
        device.getQueueFamilies().graphicsFamily.value(),
        device.getQueueFamilies().transferFamily.value()
    };
    if (device.hasDedicatedTransfer()) {
        bufCI.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        bufCI.queueFamilyIndexCount = 2;
        bufCI.pQueueFamilyIndices   = familyIndices;
    } else {
        bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VmaAllocationCreateInfo targetAllocCI{};
    targetAllocCI.usage = VMA_MEMORY_USAGE_AUTO;

    VK_CHECK(vmaCreateBuffer(allocator, &bufCI, &targetAllocCI,
                             &target.m_buffer, &target.m_allocation, nullptr),
             "Failed to create device-local buffer");

    // 3. One-shot command buffer for the copy — lock the transfer pool
    //    so concurrent buildScene (bg thread) and main thread don't collide.
    auto poolLock = device.lockTransferPool();

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

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos  = &cmdInfo;

    device.submitTransfer(1, &submitInfo);
    device.transferQueueWaitIdle();

    vkFreeCommandBuffers(device.getDevice(), device.getTransferCommandPool(), 1, &cmd);

    poolLock.unlock();

    return target;
}

} // namespace glory
