#include "renderer/ComputeSkinner.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace glory {

namespace {
static std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("ComputeSkinner: cannot open " + path);
    size_t sz = static_cast<size_t>(f.tellg());
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
}
static VkShaderModule makeModule(VkDevice dev, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m), "ComputeSkinner shader");
    return m;
}
} // namespace

// ── Init / destroy ────────────────────────────────────────────────────────
void ComputeSkinner::init(const Device& device, uint32_t framesInFlight) {
    m_device      = &device;
    m_vkDevice    = device.getDevice();
    m_framesCount = framesInFlight;
    createPipeline();
    createDescriptorPool(m_maxBatchesPerFrame, framesInFlight);
    spdlog::info("ComputeSkinner: ready ({} max batches/frame × {} frames)",
                 m_maxBatchesPerFrame, framesInFlight);
}

void ComputeSkinner::destroy() {
    if (!m_vkDevice) return;
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

// ── Pipeline creation ─────────────────────────────────────────────────────
void ComputeSkinner::createPipeline() {
    // Descriptor set layout:
    // binding 0 = bind-pose SSBO (readonly)
    // binding 1 = output SSBO (writeonly)
    // binding 2 = bone matrices SSBO (readonly)
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (int i = 0; i < 3; ++i) {
        bindings[i].binding         = static_cast<uint32_t>(i);
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 3;
    lci.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_vkDevice, &lci, nullptr, &m_descLayout),
             "ComputeSkinner desc layout");

    // Push constants: uint vertexCount + uint boneBaseIndex (8 bytes)
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(uint32_t) * 2;

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &m_descLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pc;
    VK_CHECK(vkCreatePipelineLayout(m_vkDevice, &plci, nullptr, &m_pipeLayout),
             "ComputeSkinner pipeline layout");

    auto code = readFile(std::string(SHADER_DIR) + "skinning.comp.spv");
    VkShaderModule mod = makeModule(m_vkDevice, code);

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = mod;
    cpci.stage.pName  = "main";
    cpci.layout       = m_pipeLayout;
    VK_CHECK(vkCreateComputePipelines(m_vkDevice, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                      &m_pipeline),
             "ComputeSkinner pipeline");
    vkDestroyShaderModule(m_vkDevice, mod, nullptr);
}

// ── Descriptor pool ───────────────────────────────────────────────────────
void ComputeSkinner::createDescriptorPool(uint32_t maxBatches, uint32_t frames) {
    uint32_t totalSets = maxBatches * frames;
    VkDescriptorPoolSize sizes[1]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[0].descriptorCount = totalSets * 3; // 3 SSBOs per set

    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags         = 0; // sets are reset by frame-pointer, not individually freed
    pci.maxSets       = totalSets;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = sizes;
    VK_CHECK(vkCreateDescriptorPool(m_vkDevice, &pci, nullptr, &m_descPool),
             "ComputeSkinner desc pool");

    // Pre-allocate all descriptor sets upfront
    std::vector<VkDescriptorSetLayout> layouts(totalSets, m_descLayout);
    std::vector<VkDescriptorSet> allSets(totalSets);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = m_descPool;
    ai.descriptorSetCount = totalSets;
    ai.pSetLayouts        = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(m_vkDevice, &ai, allSets.data()),
             "ComputeSkinner desc sets");

    m_framePools.resize(frames);
    for (uint32_t f = 0; f < frames; ++f) {
        m_framePools[f].sets.resize(maxBatches);
        for (uint32_t s = 0; s < maxBatches; ++s)
            m_framePools[f].sets[s] = allSets[f * maxBatches + s];
        m_framePools[f].nextSet = 0;
    }
}

void ComputeSkinner::resetFrame(uint32_t frameIdx) {
    m_framePools[frameIdx].nextSet = 0;
}

VkDescriptorSet ComputeSkinner::acquireDescSet(uint32_t frameIdx) {
    FramePool& fp = m_framePools[frameIdx];
    if (fp.nextSet >= static_cast<uint32_t>(fp.sets.size()))
        throw std::runtime_error("ComputeSkinner: exceeded max batches per frame");
    return fp.sets[fp.nextSet++];
}

// ── Per-frame dispatch ────────────────────────────────────────────────────
void ComputeSkinner::dispatch(VkCommandBuffer cmd,
                              const std::vector<SkinBatch>& batches,
                              uint32_t frameIdx) {
    if (batches.empty()) return;
    resetFrame(frameIdx);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);

    for (const auto& b : batches) {
        VkDescriptorSet ds = acquireDescSet(frameIdx);

        // Write descriptor set for this batch
        VkDescriptorBufferInfo bindPoseInfo{b.bindPoseBuffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo outputInfo  {b.outputBuffer,   0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo boneInfo    {b.boneBuffer,     0, VK_WHOLE_SIZE};

        VkWriteDescriptorSet writes[3]{};
        for (int i = 0; i < 3; ++i) {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = ds;
            writes[i].dstBinding      = static_cast<uint32_t>(i);
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        writes[0].pBufferInfo = &bindPoseInfo;
        writes[1].pBufferInfo = &outputInfo;
        writes[2].pBufferInfo = &boneInfo;
        vkUpdateDescriptorSets(m_vkDevice, 3, writes, 0, nullptr);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_pipeLayout, 0, 1, &ds, 0, nullptr);

        uint32_t pc[2] = {b.vertexCount, b.boneBaseIndex};
        vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(pc), pc);

        uint32_t groups = (b.vertexCount + 63u) / 64u;
        vkCmdDispatch(cmd, groups, 1, 1);
    }
}

// ── Output barrier ────────────────────────────────────────────────────────
void ComputeSkinner::insertOutputBarrier(VkCommandBuffer cmd,
                                         const std::vector<SkinBatch>& batches) {
    if (batches.empty()) return;

    std::vector<VkBufferMemoryBarrier> barriers;
    barriers.reserve(batches.size());
    for (const auto& b : batches) {
        VkBufferMemoryBarrier bar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bar.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dstAccessMask       = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.buffer              = b.outputBuffer;
        bar.offset              = 0;
        bar.size                = VK_WHOLE_SIZE;
        barriers.push_back(bar);
    }

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0, 0, nullptr,
        static_cast<uint32_t>(barriers.size()), barriers.data(),
        0, nullptr);
}

// ── Static helper: allocate pre-skinned output buffer ─────────────────────
VkBuffer ComputeSkinner::allocateOutputBuffer(const Device& device,
                                               uint32_t vertexCount,
                                               VmaAllocation& outAlloc) {
    // Pre-skinned vertex: pos(12) + color(12) + normal(12) + uv(8) = 44 bytes
    constexpr VkDeviceSize PRESKINNED_VERTEX_STRIDE = 44;
    VkDeviceSize size = PRESKINNED_VERTEX_STRIDE * vertexCount;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VkBuffer buf;
    VK_CHECK(vmaCreateBuffer(device.getAllocator(), &bci, &aci, &buf, &outAlloc, nullptr),
             "ComputeSkinner output buffer");
    return buf;
}

} // namespace glory
