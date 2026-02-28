#include "renderer/ParticleSystem.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace glory {

ParticleSystem::ParticleSystem(const Device& device, VkRenderPass renderPass,
                               uint32_t maxParticles)
    : m_device(device)
    , m_allocator(device.getAllocator())
    , m_maxParticles(maxParticles)
{
    // Create particle SSBO (host-visible for CPU emission + GPU compute read/write)
    {
        VkBufferCreateInfo bufCI{};
        bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufCI.size  = maxParticles * sizeof(GPUParticle);
        bufCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        VmaAllocationCreateInfo allocCI{};
        allocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocInfo{};
        VK_CHECK(vmaCreateBuffer(m_allocator, &bufCI, &allocCI,
                                 &m_particleBuffer, &m_particleAlloc, &allocInfo),
                 "Failed to create particle SSBO");
        m_particleMapped = allocInfo.pMappedData;

        // Zero-initialize all particles (lifetime 0 = dead)
        std::memset(m_particleMapped, 0, maxParticles * sizeof(GPUParticle));
    }

    // Create vertex output buffer (GPU-local, written by compute, read as vertex)
    {
        VkBufferCreateInfo bufCI{};
        bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufCI.size  = maxParticles * sizeof(ParticleVertex);
        bufCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        VmaAllocationCreateInfo allocCI{};
        allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VK_CHECK(vmaCreateBuffer(m_allocator, &bufCI, &allocCI,
                                 &m_vertexBuffer, &m_vertexAlloc, nullptr),
                 "Failed to create particle vertex buffer");
    }

    // Create atomic counter buffer (host-visible for CPU reset + GPU atomic ops)
    {
        VkBufferCreateInfo bufCI{};
        bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufCI.size  = sizeof(uint32_t);
        bufCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        VmaAllocationCreateInfo allocCI{};
        allocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocInfo{};
        VK_CHECK(vmaCreateBuffer(m_allocator, &bufCI, &allocCI,
                                 &m_counterBuffer, &m_counterAlloc, &allocInfo),
                 "Failed to create particle counter buffer");
        m_counterMapped = allocInfo.pMappedData;
    }

    createComputePipeline();
    createGraphicsPipeline(renderPass);
    spdlog::info("Particle system created (max {} particles, GPU compute)", maxParticles);
}

ParticleSystem::~ParticleSystem() {
    VkDevice dev = m_device.getDevice();
    if (m_computePipeline)   vkDestroyPipeline(dev, m_computePipeline, nullptr);
    if (m_computePipeLayout) vkDestroyPipelineLayout(dev, m_computePipeLayout, nullptr);
    if (m_computeDescPool)   vkDestroyDescriptorPool(dev, m_computeDescPool, nullptr);
    if (m_computeDescLayout) vkDestroyDescriptorSetLayout(dev, m_computeDescLayout, nullptr);
    if (m_pipeline)          vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout)    vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_particleBuffer)    vmaDestroyBuffer(m_allocator, m_particleBuffer, m_particleAlloc);
    if (m_vertexBuffer)      vmaDestroyBuffer(m_allocator, m_vertexBuffer, m_vertexAlloc);
    if (m_counterBuffer)     vmaDestroyBuffer(m_allocator, m_counterBuffer, m_counterAlloc);
}

void ParticleSystem::setEmitter(glm::vec3 pos, glm::vec3 baseColor, float emitRate) {
    m_emitterPos = pos;
    m_baseColor  = baseColor;
    m_emitRate   = emitRate;
}

float ParticleSystem::randFloat(float lo, float hi) {
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(m_rng);
}

void ParticleSystem::emit(uint32_t count) {
    auto* particles = static_cast<GPUParticle*>(m_particleMapped);

    // Find dead slots and emit into them
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < m_maxParticles && emitted < count; ++i) {
        if (particles[i].posAndLifetime.w > 0.0f) continue; // alive, skip

        float maxLife = randFloat(0.8f, 2.5f);
        float brightness = randFloat(0.7f, 1.3f);

        particles[i].posAndLifetime = glm::vec4(
            m_emitterPos.x + randFloat(-0.15f, 0.15f),
            m_emitterPos.y + randFloat(-0.05f, 0.05f),
            m_emitterPos.z + randFloat(-0.15f, 0.15f),
            maxLife  // lifetime
        );
        particles[i].velAndMaxLife = glm::vec4(
            randFloat(-0.4f, 0.4f),
            randFloat(0.8f, 2.5f),
            randFloat(-0.4f, 0.4f),
            maxLife  // maxLifetime
        );
        particles[i].color = glm::vec4(m_baseColor * brightness, 1.0f);
        ++emitted;
    }
}

void ParticleSystem::update(float dt) {
    // Read back alive count from previous frame's compute dispatch
    // (safe because fence wait in drawFrame ensures GPU work is complete)
    std::memcpy(&m_aliveCount, m_counterMapped, sizeof(uint32_t));

    // Emit new particles (CPU-side, writes to mapped SSBO)
    m_emitAccum += m_emitRate * dt;
    uint32_t toEmit = static_cast<uint32_t>(m_emitAccum);
    m_emitAccum -= static_cast<float>(toEmit);
    emit(toEmit);

    // Reset atomic counter to 0 before compute dispatch
    uint32_t zero = 0;
    std::memcpy(m_counterMapped, &zero, sizeof(uint32_t));
}

void ParticleSystem::dispatchCompute(VkCommandBuffer cmd, float dt) {
    // Bind compute pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_computePipeLayout, 0, 1, &m_computeDescSet, 0, nullptr);

    // Push constants: deltaTime, gravity, maxParticles
    struct ComputePC {
        float    deltaTime;
        float    gravity;
        uint32_t maxParticles;
    } pc;
    pc.deltaTime    = dt;
    pc.gravity      = -1.2f;
    pc.maxParticles = m_maxParticles;
    vkCmdPushConstants(cmd, m_computePipeLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    // Dispatch: ceil(maxParticles / 64)
    uint32_t groupCount = (m_maxParticles + 63) / 64;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // Memory barrier: compute shader writes → vertex attribute reads
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void ParticleSystem::record(VkCommandBuffer cmd, const glm::mat4& viewProj,
                            float particleSize) {
    if (m_aliveCount == 0) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    struct PushData {
        glm::mat4 viewProj;
        float     particleSize;
    } pc;
    pc.viewProj     = viewProj;
    pc.particleSize = particleSize;
    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer, &offset);
    vkCmdDraw(cmd, m_aliveCount, 1, 0, 0);
}

// ── Compute pipeline ────────────────────────────────────────────────────────
void ParticleSystem::createComputePipeline() {
    VkDevice dev = m_device.getDevice();

    // Descriptor set layout: 3 SSBOs
    VkDescriptorSetLayoutBinding bindings[3]{};
    // Binding 0: Particle SSBO (read/write)
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 1: Vertex output SSBO (write)
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 2: Atomic counter SSBO (read/write)
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 3;
    layoutCI.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_computeDescLayout),
             "Failed to create compute descriptor set layout");

    // Descriptor pool
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 3;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_computeDescPool),
             "Failed to create compute descriptor pool");

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_computeDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_computeDescLayout;
    VK_CHECK(vkAllocateDescriptorSets(dev, &allocInfo, &m_computeDescSet),
             "Failed to allocate compute descriptor set");

    // Write descriptor set
    VkDescriptorBufferInfo particleBufInfo{};
    particleBufInfo.buffer = m_particleBuffer;
    particleBufInfo.offset = 0;
    particleBufInfo.range  = m_maxParticles * sizeof(GPUParticle);

    VkDescriptorBufferInfo vertexBufInfo{};
    vertexBufInfo.buffer = m_vertexBuffer;
    vertexBufInfo.offset = 0;
    vertexBufInfo.range  = m_maxParticles * sizeof(ParticleVertex);

    VkDescriptorBufferInfo counterBufInfo{};
    counterBufInfo.buffer = m_counterBuffer;
    counterBufInfo.offset = 0;
    counterBufInfo.range  = sizeof(uint32_t);

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_computeDescSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo     = &particleBufInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_computeDescSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo     = &vertexBufInfo;

    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = m_computeDescSet;
    writes[2].dstBinding      = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo     = &counterBufInfo;

    vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);

    // Pipeline layout
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(float) * 2 + sizeof(uint32_t); // 12 bytes

    VkPipelineLayoutCreateInfo pipeLayoutCI{};
    pipeLayoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutCI.setLayoutCount         = 1;
    pipeLayoutCI.pSetLayouts            = &m_computeDescLayout;
    pipeLayoutCI.pushConstantRangeCount = 1;
    pipeLayoutCI.pPushConstantRanges    = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &pipeLayoutCI, nullptr, &m_computePipeLayout),
             "Failed to create compute pipeline layout");

    // Load compute shader
    auto readFile = [](const std::string& path) -> std::vector<char> {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("Failed to open shader: " + path);
        size_t sz = static_cast<size_t>(file.tellg());
        std::vector<char> buf(sz);
        file.seekg(0);
        file.read(buf.data(), static_cast<std::streamsize>(sz));
        return buf;
    };

    auto compCode = readFile(std::string(SHADER_DIR) + "particle_sim.comp.spv");

    VkShaderModuleCreateInfo modCI{};
    modCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    modCI.codeSize = compCode.size();
    modCI.pCode    = reinterpret_cast<const uint32_t*>(compCode.data());
    VkShaderModule compMod;
    VK_CHECK(vkCreateShaderModule(dev, &modCI, nullptr, &compMod),
             "Failed to create compute shader module");

    VkComputePipelineCreateInfo pipeCI{};
    pipeCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeCI.layout = m_computePipeLayout;
    pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module = compMod;
    pipeCI.stage.pName  = "main";

    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_computePipeline),
             "Failed to create compute pipeline");

    vkDestroyShaderModule(dev, compMod, nullptr);
    spdlog::info("Particle compute pipeline created");
}

// ── Graphics pipeline ───────────────────────────────────────────────────────
void ParticleSystem::createGraphicsPipeline(VkRenderPass renderPass) {
    VkDevice dev = m_device.getDevice();

    auto readFile = [](const std::string& path) -> std::vector<char> {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("Failed to open shader: " + path);
        size_t sz = static_cast<size_t>(file.tellg());
        std::vector<char> buf(sz);
        file.seekg(0);
        file.read(buf.data(), static_cast<std::streamsize>(sz));
        return buf;
    };

    auto makeModule = [&](const std::vector<char>& code) {
        VkShaderModuleCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size();
        ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod;
        VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod), "shader module");
        return mod;
    };

    auto vertCode = readFile(std::string(SHADER_DIR) + "particle.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "particle.frag.spv");
    VkShaderModule vertMod = makeModule(vertCode);
    VkShaderModule fragMod = makeModule(fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    // Vertex input: position (vec3) + color (vec4)
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(ParticleVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding  = 0;
    attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset   = offsetof(ParticleVertex, position);
    attrs[1].location = 1;
    attrs[1].binding  = 0;
    attrs[1].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[1].offset   = offsetof(ParticleVertex, color);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dynStates;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable         = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttachment;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(glm::mat4) + sizeof(float);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pushRange;

    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout),
             "Failed to create particle pipeline layout");

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.stageCount          = 2;
    pipelineCI.pStages             = stages;
    pipelineCI.pVertexInputState   = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState      = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState   = &multisample;
    pipelineCI.pDepthStencilState  = &depthStencil;
    pipelineCI.pColorBlendState    = &colorBlend;
    pipelineCI.pDynamicState       = &dynState;
    pipelineCI.layout              = m_pipelineLayout;
    pipelineCI.renderPass          = renderPass;
    pipelineCI.subpass             = 0;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &m_pipeline),
             "Failed to create particle pipeline");

    vkDestroyShaderModule(dev, fragMod, nullptr);
    vkDestroyShaderModule(dev, vertMod, nullptr);
    spdlog::info("Particle graphics pipeline created");
}

} // namespace glory
