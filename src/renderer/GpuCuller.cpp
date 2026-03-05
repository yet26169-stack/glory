#include "renderer/GpuCuller.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_access.hpp>

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace glory {

// ── Shader helper ─────────────────────────────────────────────────────────
static std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("GpuCuller: cannot open " + path);
    size_t sz = static_cast<size_t>(f.tellg());
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
}

static VkShaderModule makeModule(VkDevice dev, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m), "GpuCuller shader module");
    return m;
}

// ── Frustum extraction (Gribb-Hartmann method, column-major GLM) ──────────
glm::vec4 GpuCuller::normalizePlane(glm::vec4 p) {
    float len = glm::length(glm::vec3(p));
    return (len > 1e-6f) ? p / len : p;
}

void GpuCuller::extractFrustumPlanes(const glm::mat4& vp, glm::vec4 out[6]) {
    // Row vectors of the combined VP matrix
    glm::vec4 r0 = glm::row(vp, 0);
    glm::vec4 r1 = glm::row(vp, 1);
    glm::vec4 r2 = glm::row(vp, 2);
    glm::vec4 r3 = glm::row(vp, 3);

    out[0] = normalizePlane(r3 + r0); // left
    out[1] = normalizePlane(r3 - r0); // right
    out[2] = normalizePlane(r3 + r1); // bottom
    out[3] = normalizePlane(r3 - r1); // top
    out[4] = normalizePlane(r3 + r2); // near  (Vulkan: z in [0,1])
    out[5] = normalizePlane(r3 - r2); // far
}

// ── Initialisation ────────────────────────────────────────────────────────
void GpuCuller::init(const Device& device, uint32_t framesInFlight,
                     uint32_t maxObjects) {
    m_device      = &device;
    m_vkDevice    = device.getDevice();
    m_maxObjects  = maxObjects;
    m_framesCount = framesInFlight;

    createPipeline();
    createDescriptorPool(framesInFlight);
    createFrameData(framesInFlight);
    spdlog::info("GpuCuller initialised ({} max objects, {} frames)", maxObjects, framesInFlight);
}

void GpuCuller::createPipeline() {
    // Descriptor set layout: bindings 0,1,2,3
    VkDescriptorSetLayoutBinding bindings[4]{};
    // 0 — object SSBO (readonly input)
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    // 1 — output draw commands SSBO (readwrite)
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    // 2 — frustum UBO
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    // 3 — output instance buffer SSBO (writeonly)
    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 4;
    lci.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_vkDevice, &lci, nullptr, &m_descLayout),
             "GpuCuller desc layout");

    VkPipelineLayoutCreateInfo plci{};
    plci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts    = &m_descLayout;
    VK_CHECK(vkCreatePipelineLayout(m_vkDevice, &plci, nullptr, &m_pipeLayout),
             "GpuCuller pipeline layout");

    auto code   = readFile(std::string(SHADER_DIR) + "cull.comp.spv");
    VkShaderModule mod = makeModule(m_vkDevice, code);

    VkComputePipelineCreateInfo cpci{};
    cpci.sType               = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType         = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage         = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module        = mod;
    cpci.stage.pName         = "main";
    cpci.layout              = m_pipeLayout;
    VK_CHECK(vkCreateComputePipelines(m_vkDevice, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                       &m_pipeline),
             "GpuCuller compute pipeline");
    vkDestroyShaderModule(m_vkDevice, mod, nullptr);
}

void GpuCuller::createDescriptorPool(uint32_t frames) {
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[0].descriptorCount = frames * 3; // object + output cmds + output instances
    sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = frames;     // frustum UBO

    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets       = frames;
    pci.poolSizeCount = 2;
    pci.pPoolSizes    = sizes;
    VK_CHECK(vkCreateDescriptorPool(m_vkDevice, &pci, nullptr, &m_descPool),
             "GpuCuller desc pool");
}

void GpuCuller::createFrameData(uint32_t frames) {
    m_frames.resize(frames);

    VkDeviceSize objectBufSize  = sizeof(CullObject) * m_maxObjects;
    // Output draw commands: uint drawCount + VkDrawIndexedIndirectCommand × maxObjects
    VkDeviceSize outputBufSize  = sizeof(uint32_t) +
                                  sizeof(uint32_t) * 5 * m_maxObjects; // 5 uints per cmd
    // Output instance data: full InstanceData layout
    // model(64) + normalMatrix(64) + tint(16) + params(16) + texIndices(16) = 176 bytes
    VkDeviceSize instanceBufSize = 176u * m_maxObjects;
    VkDeviceSize frustumBufSize = sizeof(FrustumUBO);

    VmaAllocator allocator = m_device->getAllocator();

    for (uint32_t i = 0; i < frames; ++i) {
        FrameData& fd = m_frames[i];

        fd.objectBuffer = Buffer(allocator, objectBufSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);

        fd.outputBuffer = Buffer(allocator, outputBufSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY);

        fd.instanceBuffer = Buffer(allocator, instanceBufSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY);

        fd.frustumBuffer = Buffer(allocator, frustumBufSize,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);

        // Allocate + write descriptor set
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = m_descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &m_descLayout;
        VK_CHECK(vkAllocateDescriptorSets(m_vkDevice, &ai, &fd.descSet),
                 "GpuCuller desc set");

        VkDescriptorBufferInfo objInfo{fd.objectBuffer.getBuffer(),   0, objectBufSize};
        VkDescriptorBufferInfo outInfo{fd.outputBuffer.getBuffer(),   0, outputBufSize};
        VkDescriptorBufferInfo frsInfo{fd.frustumBuffer.getBuffer(),  0, frustumBufSize};
        VkDescriptorBufferInfo instInfo{fd.instanceBuffer.getBuffer(), 0, instanceBufSize};

        VkWriteDescriptorSet writes[4]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     fd.descSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     nullptr, &objInfo, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     fd.descSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     nullptr, &outInfo, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     fd.descSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                     nullptr, &frsInfo, nullptr};
        writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                     fd.descSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     nullptr, &instInfo, nullptr};
        vkUpdateDescriptorSets(m_vkDevice, 4, writes, 0, nullptr);
    }
}

// ── Object management ─────────────────────────────────────────────────────
uint32_t GpuCuller::addObject(const CullObject& obj) {
    uint32_t slot = static_cast<uint32_t>(m_objects.size());
    m_objects.push_back(obj);
    return slot;
}

void GpuCuller::updateObject(uint32_t slot, const CullObject& obj) {
    if (slot < m_objects.size()) m_objects[slot] = obj;
}

void GpuCuller::setObjects(const std::vector<CullObject>& objs) {
    m_objects = objs;
}

// ── Per-frame dispatch ────────────────────────────────────────────────────
void GpuCuller::dispatch(VkCommandBuffer cmd, const glm::mat4& vp,
                         uint32_t frameIdx) {
    if (m_objects.empty()) return;
    FrameData& fd = m_frames[frameIdx];

    uint32_t count = static_cast<uint32_t>(std::min(m_objects.size(),
                                                     static_cast<size_t>(m_maxObjects)));

    // Upload object data
    std::memcpy(fd.objectBuffer.map(), m_objects.data(), count * sizeof(CullObject));

    // Upload frustum planes
    FrustumUBO fruUBO{};
    extractFrustumPlanes(vp, fruUBO.planes);
    fruUBO.objectCount = count;
    fruUBO.maxCommands = m_maxObjects;
    std::memcpy(fd.frustumBuffer.map(), &fruUBO, sizeof(FrustumUBO));

    // Reset draw count to 0 (only the leading uint, not the whole buffer)
    vkCmdFillBuffer(cmd, fd.outputBuffer.getBuffer(), 0, sizeof(uint32_t), 0u);

    // Memory barrier: fill → compute shader write
    VkBufferMemoryBarrier fillBarrier{};
    fillBarrier.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fillBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    fillBarrier.buffer        = fd.outputBuffer.getBuffer();
    fillBarrier.offset        = 0;
    fillBarrier.size          = VK_WHOLE_SIZE;
    fillBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fillBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 1, &fillBarrier, 0, nullptr);

    // Dispatch compute
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipeLayout, 0, 1, &fd.descSet, 0, nullptr);
    uint32_t groups = (count + 63u) / 64u;
    vkCmdDispatch(cmd, groups, 1, 1);

    // Barrier: compute write → indirect draw read + vertex attribute read (instance buffer)
    VkBufferMemoryBarrier compBarriers[2]{};
    // Draw command buffer: compute write → indirect draw read
    compBarriers[0].sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    compBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    compBarriers[0].dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    compBarriers[0].buffer        = fd.outputBuffer.getBuffer();
    compBarriers[0].offset        = 0;
    compBarriers[0].size          = VK_WHOLE_SIZE;
    compBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    compBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    // Instance buffer: compute write → vertex attribute read
    compBarriers[1].sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    compBarriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    compBarriers[1].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    compBarriers[1].buffer        = fd.instanceBuffer.getBuffer();
    compBarriers[1].offset        = 0;
    compBarriers[1].size          = VK_WHOLE_SIZE;
    compBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    compBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0, 0, nullptr, 2, compBarriers, 0, nullptr);
}

VkBuffer GpuCuller::outputBuffer(uint32_t frameIdx) const {
    return m_frames[frameIdx].outputBuffer.getBuffer();
}

VkBuffer GpuCuller::instanceBuffer(uint32_t frameIdx) const {
    return m_frames[frameIdx].instanceBuffer.getBuffer();
}

// ── Cleanup ───────────────────────────────────────────────────────────────
void GpuCuller::destroy() {
    if (!m_vkDevice) return;

    for (auto& fd : m_frames) {
        fd.objectBuffer.destroy();
        fd.outputBuffer.destroy();
        fd.instanceBuffer.destroy();
        fd.frustumBuffer.destroy();
    }
    m_frames.clear();

    if (m_pipeline)   vkDestroyPipeline(m_vkDevice, m_pipeline, nullptr);
    if (m_pipeLayout) vkDestroyPipelineLayout(m_vkDevice, m_pipeLayout, nullptr);
    if (m_descPool)   vkDestroyDescriptorPool(m_vkDevice, m_descPool, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(m_vkDevice, m_descLayout, nullptr);

    m_pipeline   = VK_NULL_HANDLE;
    m_pipeLayout = VK_NULL_HANDLE;
    m_descPool   = VK_NULL_HANDLE;
    m_descLayout = VK_NULL_HANDLE;
    m_vkDevice   = VK_NULL_HANDLE;
}

} // namespace glory
