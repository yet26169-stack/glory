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
    : m_device(device), m_allocator(device.getAllocator()), m_maxParticles(maxParticles)
{
    // 1. Create SSBO for particle data (CPU_TO_GPU, persistently mapped)
    m_particleBuffer = Buffer(m_allocator, sizeof(GPUParticle) * maxParticles,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU);
    
    // Initialize particles to dead
    GPUParticle* p = static_cast<GPUParticle*>(m_particleBuffer.map());
    for (uint32_t i = 0; i < maxParticles; ++i) {
        p[i].posAndLifetime.w = -1.0f; // negative life = dead
    }

    // 2. Create output vertex buffer (GPU_ONLY)
    m_vertexBuffer = Buffer(m_allocator, sizeof(ParticleVertex) * maxParticles,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VMA_MEMORY_USAGE_GPU_ONLY);

    // 3. Create atomic counter buffer (CPU_TO_GPU, persistently mapped)
    m_counterBuffer = Buffer(m_allocator, sizeof(uint32_t),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            VMA_MEMORY_USAGE_CPU_TO_GPU);
    *static_cast<uint32_t*>(m_counterBuffer.map()) = 0;

    createComputePipeline();
    createGraphicsPipeline(renderPass);
    spdlog::info("ParticleSystem initialized (max particles: {})", maxParticles);
}

ParticleSystem::~ParticleSystem() {
    VkDevice dev = m_device.getDevice();
    if (m_computePipeline)   vkDestroyPipeline(dev, m_computePipeline, nullptr);
    if (m_computePipeLayout) vkDestroyPipelineLayout(dev, m_computePipeLayout, nullptr);
    if (m_computeDescPool)   vkDestroyDescriptorPool(dev, m_computeDescPool, nullptr);
    if (m_computeDescLayout) vkDestroyDescriptorSetLayout(dev, m_computeDescLayout, nullptr);

    if (m_pipeline)          vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout)    vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
}

void ParticleSystem::setEmitter(glm::vec3 pos, glm::vec3 baseColor, float emitRate) {
    m_emitterPos = pos;
    m_baseColor  = baseColor;
    m_emitRate   = emitRate;
}

void ParticleSystem::setCameraPosition(const glm::vec3& camPos,
                                       float fullRateDistance,
                                       float cullDistance) {
    float dist = glm::distance(camPos, m_emitterPos);
    if (dist > cullDistance) {
        m_emitRateScale = 0.0f;
    } else if (dist <= fullRateDistance) {
        m_emitRateScale = 1.0f;
    } else {
        // Linear falloff from 1 to 0
        m_emitRateScale = 1.0f - (dist - fullRateDistance) / (cullDistance - fullRateDistance);
    }
}

void ParticleSystem::update(float dt) {
    // Read alive count from atomic counter (from last frame's dispatch)
    uint32_t* counter = static_cast<uint32_t*>(m_counterBuffer.map());
    m_aliveCount = *counter;
    *counter = 0; // reset for next dispatch

    // Emit new particles
    m_emitAccum += m_emitRate * m_emitRateScale * dt;
    uint32_t toEmit = static_cast<uint32_t>(std::floor(m_emitAccum));
    m_emitAccum -= static_cast<float>(toEmit);

    if (toEmit > 0) {
        emit(toEmit);
    }
}

void ParticleSystem::emit(uint32_t count) {
    GPUParticle* particles = static_cast<GPUParticle*>(m_particleBuffer.map());
    uint32_t emitted = 0;

    for (uint32_t i = 0; i < m_maxParticles && emitted < count; ++i) {
        if (particles[i].posAndLifetime.w < 0.0f) {
            // Found a dead particle slot
            particles[i].posAndLifetime = glm::vec4(m_emitterPos, 0.0f);
            
            float vx = randFloat(-1.0f, 1.0f);
            float vy = randFloat(2.0f, 5.0f);
            float vz = randFloat(-1.0f, 1.0f);
            float life = randFloat(1.5f, 3.0f);
            
            particles[i].velAndMaxLife = glm::vec4(vx, vy, vz, life);
            particles[i].color = glm::vec4(m_baseColor, 1.0f);
            
            emitted++;
        }
    }
}

float ParticleSystem::randFloat(float lo, float hi) {
    std::uniform_real_distribution<float> d(lo, hi);
    return d(m_rng);
}

void ParticleSystem::dispatchCompute(VkCommandBuffer cmd, float dt) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeLayout,
                            0, 1, &m_computeDescSet, 0, nullptr);

    struct {
        float dt;
        float gravity;
        uint32_t max;
    } pc = { dt, -9.81f, m_maxParticles };

    vkCmdPushConstants(cmd, m_computePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t groupCount = (m_maxParticles + 63) / 64; // local_size_x = 64
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // Barrier: ensure compute writes to vertex buffer are finished before graphics reads
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    barrier.buffer = m_vertexBuffer.getBuffer();
    barrier.offset = 0;
    barrier.size   = VK_WHOLE_SIZE;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr,
                         1, &barrier, 0, nullptr);
}

void ParticleSystem::record(VkCommandBuffer cmd, const glm::mat4& viewProj, float particleSize) {
    if (m_aliveCount == 0) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    struct {
        glm::mat4 viewProj;
        float size;
    } push;
    push.viewProj = viewProj;
    push.size     = particleSize;

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(push), &push);

    VkBuffer buf = m_vertexBuffer.getBuffer();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);
    vkCmdDraw(cmd, m_maxParticles, 1, 0, 0);
}

void ParticleSystem::createComputePipeline() {
    VkDevice dev = m_device.getDevice();

    // Descriptor layout: 0 = particles, 1 = vertex output, 2 = counter
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = 3;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_computeDescLayout), "compute layout");

    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_computeDescPool), "compute pool");

    VkDescriptorSetAllocateInfo setAI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAI.descriptorPool = m_computeDescPool;
    setAI.descriptorSetCount = 1;
    setAI.pSetLayouts = &m_computeDescLayout;
    VK_CHECK(vkAllocateDescriptorSets(dev, &setAI, &m_computeDescSet), "compute set");

    VkDescriptorBufferInfo bufInfos[3]{};
    bufInfos[0] = {m_particleBuffer.getBuffer(), 0, VK_WHOLE_SIZE};
    bufInfos[1] = {m_vertexBuffer.getBuffer(), 0, VK_WHOLE_SIZE};
    bufInfos[2] = {m_counterBuffer.getBuffer(), 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet writes[3]{};
    for(int i=0; i<3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_computeDescSet;
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &bufInfos[i];
    }
    vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.size = sizeof(float) * 3; // deltaTime, gravity, maxParticles
    VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &m_computeDescLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pc;
    VK_CHECK(vkCreatePipelineLayout(dev, &plCI, nullptr, &m_computePipeLayout), "compute pipe layout");

    auto readFile = [](const std::string& path) {
        std::ifstream f(path, std::ios::ate | std::ios::binary);
        size_t sz = (size_t)f.tellg();
        std::vector<char> buf(sz);
        f.seekg(0); f.read(buf.data(), sz);
        return buf;
    };
    auto code = readFile(std::string(SHADER_DIR) + "particle_sim.comp.spv");
    VkShaderModuleCreateInfo smCI{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smCI.codeSize = code.size();
    smCI.pCode = (const uint32_t*)code.data();
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(dev, &smCI, nullptr, &mod), "particle sim shader");

    VkComputePipelineCreateInfo cpCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpCI.layout = m_computePipeLayout;
    cpCI.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpCI.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpCI.stage.module = mod;
    cpCI.stage.pName = "main";
    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpCI, nullptr, &m_computePipeline), "particle compute pipeline");
    vkDestroyShaderModule(dev, mod, nullptr);
}

void ParticleSystem::createGraphicsPipeline(VkRenderPass renderPass) {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.size = sizeof(glm::mat4) + sizeof(float);
    VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pc;
    VK_CHECK(vkCreatePipelineLayout(dev, &plCI, nullptr, &m_pipelineLayout), "particle graphics layout");

    auto readFile = [](const std::string& path) {
        std::ifstream f(path, std::ios::ate | std::ios::binary);
        size_t sz = (size_t)f.tellg();
        std::vector<char> buf(sz);
        f.seekg(0); f.read(buf.data(), sz);
        return buf;
    };
    auto vcode = readFile(std::string(SHADER_DIR) + "particle.vert.spv");
    auto fcode = readFile(std::string(SHADER_DIR) + "particle.frag.spv");
    
    VkShaderModuleCreateInfo smCI{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smCI.codeSize = vcode.size();
    smCI.pCode = (const uint32_t*)vcode.data();
    VkShaderModule vertMod;
    vkCreateShaderModule(dev, &smCI, nullptr, &vertMod);
    
    smCI.codeSize = fcode.size();
    smCI.pCode = (const uint32_t*)fcode.data();
    VkShaderModule fragMod;
    vkCreateShaderModule(dev, &smCI, nullptr, &fragMod);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main"};

    VkVertexInputBindingDescription binding{0, sizeof(ParticleVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ParticleVertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleVertex, color)};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE; ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cbA{};
    cbA.blendEnable = VK_TRUE;
    cbA.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cbA.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cbA.colorBlendOp = VK_BLEND_OP_ADD;
    cbA.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbA.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cbA.alphaBlendOp = VK_BLEND_OP_ADD;
    cbA.colorWriteMask = 0xF;

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cbA;

    VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyns;

    VkGraphicsPipelineCreateInfo gpCI{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpCI.stageCount = 2; gpCI.pStages = stages; gpCI.pVertexInputState = &vi;
    gpCI.pInputAssemblyState = &ia; gpCI.pViewportState = &vp; gpCI.pRasterizationState = &rs;
    gpCI.pMultisampleState = &ms; gpCI.pDepthStencilState = &ds; gpCI.pColorBlendState = &cb;
    gpCI.pDynamicState = &dy; gpCI.layout = m_pipelineLayout; gpCI.renderPass = renderPass;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &m_pipeline), "particle graphics pipeline");

    vkDestroyShaderModule(dev, fragMod, nullptr);
    vkDestroyShaderModule(dev, vertMod, nullptr);
}

} // namespace glory
