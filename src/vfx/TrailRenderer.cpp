#include "vfx/TrailRenderer.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <algorithm>

namespace glory {

// ── Helper ───────────────────────────────────────────────────────────────────
static std::vector<char> readFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Failed to open file: " + filepath);
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    return buffer;
}

TrailRenderer::TrailRenderer(const Device& device, VkRenderPass renderPass)
    : m_device(device), m_renderPass(renderPass) 
{
    m_whiteTexture = std::make_unique<Texture>(Texture::createDefault(device));
    createDescriptorLayout();
    createPipelines();

    // Pool for trail descriptor sets
    VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_ACTIVE_TRAILS },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_ACTIVE_TRAILS }
    };
    VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolCI.maxSets = MAX_ACTIVE_TRAILS;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = sizes;
    poolCI.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    VK_CHECK(vkCreateDescriptorPool(device.getDevice(), &poolCI, nullptr, &m_descPool), "Create Trail desc pool");
}

TrailRenderer::~TrailRenderer() {
    VkDevice dev = m_device.getDevice();
    vkDeviceWaitIdle(dev);
    
    if (m_alphaPipeline) vkDestroyPipeline(dev, m_alphaPipeline, nullptr);
    if (m_additivePipeline) vkDestroyPipeline(dev, m_additivePipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
}

void TrailRenderer::registerTrail(const TrailDef& def) {
    m_defs[def.id] = def;
}

uint32_t TrailRenderer::spawn(const std::string& trailDefId, glm::vec3 startPos) {
    auto it = m_defs.find(trailDefId);
    if (it == m_defs.end()) {
        spdlog::warn("TrailRenderer: unknown def '{}'", trailDefId);
        return INVALID_TRAIL_HANDLE;
    }

    auto inst = std::make_unique<TrailInstance>();
    inst->handle = m_nextHandle++;
    inst->def = &it->second;
    inst->lastHeadPos = startPos;
    inst->texture = m_whiteTexture.get(); // fallback

    // Create SSBO
    inst->ssbo = Buffer(m_device.getAllocator(),
                        sizeof(GpuTrailPoint) * MAX_TRAIL_POINTS,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VMA_MEMORY_USAGE_CPU_TO_GPU);

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_device.getDevice(), &allocInfo, &inst->descSet), "Alloc Trail desc set");

    // Write descriptor set
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = inst->ssbo.getBuffer();
    bufInfo.offset = 0;
    bufInfo.range = VK_WHOLE_SIZE;

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView = inst->texture->getImageView();
    imgInfo.sampler = inst->texture->getSampler();

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = inst->descSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &bufInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = inst->descSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 2, writes, 0, nullptr);

    // Initial point
    GpuTrailPoint p{};
    p.posWidth = glm::vec4(startPos, inst->def->widthStart);
    p.colorAge = inst->def->colorStart;
    p.colorAge.a = 0.0f;
    inst->points.push_back(p);

    m_activeTrails.push_back(std::move(inst));
    return m_nextHandle - 1;
}

void TrailRenderer::updateHead(uint32_t handle, glm::vec3 newHeadPos) {
    for (auto& inst : m_activeTrails) {
        if (inst->handle == handle) {
            inst->lastHeadPos = newHeadPos;
            if (!inst->points.empty()) {
                inst->points.front().posWidth = glm::vec4(newHeadPos, inst->def->widthStart);
            }
            return;
        }
    }
}

void TrailRenderer::detach(uint32_t handle) {
    for (auto& inst : m_activeTrails) {
        if (inst->handle == handle) {
            inst->detached = true;
            return;
        }
    }
}

void TrailRenderer::update(float dt) {
    for (auto it = m_activeTrails.begin(); it != m_activeTrails.end(); ) {
        auto& inst = **it;
        
        // Update ages
        for (auto& p : inst.points) {
            p.colorAge.a += inst.def->fadeSpeed * dt;
        }

        // Remove old points
        while (!inst.points.empty() && inst.points.back().colorAge.a >= 1.0f) {
            inst.points.pop_back();
        }

        // Emit new points if not detached
        if (!inst.detached) {
            inst.emitAccum += dt;
            if (inst.emitAccum >= inst.def->emitInterval) {
                inst.emitAccum = 0.0f;
                GpuTrailPoint p{};
                p.posWidth = glm::vec4(inst.lastHeadPos, inst.def->widthStart);
                p.colorAge = inst.def->colorStart;
                p.colorAge.a = 0.0f;
                inst.points.push_front(p);
                
                if (inst.points.size() > MAX_TRAIL_POINTS) {
                    inst.points.pop_back();
                }
            }
        }

        // Update widths and colors for all points (linear interpolation based on age)
        for (auto& p : inst.points) {
            float t = p.colorAge.a;
            p.posWidth.w = glm::mix(inst.def->widthStart, inst.def->widthEnd, t);
            glm::vec4 c = glm::mix(inst.def->colorStart, inst.def->colorEnd, t);
            p.colorAge.r = c.r; p.colorAge.g = c.g; p.colorAge.b = c.b;
        }

        if (inst.points.empty() && inst.detached) {
            it = m_activeTrails.erase(it);
        } else {
            updateInstanceBuffer(inst);
            ++it;
        }
    }
}

void TrailRenderer::updateInstanceBuffer(TrailInstance& inst) {
    if (inst.points.empty()) return;
    void* data = inst.ssbo.map();
    size_t count = std::min((size_t)MAX_TRAIL_POINTS, inst.points.size());
    std::copy(inst.points.begin(), inst.points.begin() + count, (GpuTrailPoint*)data);
    inst.ssbo.unmap();
}

void TrailRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                           const glm::vec3& camRight, const glm::vec3& camUp) {
    if (m_activeTrails.empty()) return;

    struct TrailPC {
        glm::mat4 viewProj;
        glm::vec4 camRight;
        glm::vec4 camUp;
        uint32_t  pointCount;
        uint32_t  headIndex;
        float widthStart;
        float widthEnd;
    } pc;

    pc.viewProj = viewProj;
    pc.camRight = glm::vec4(camRight, 0.0f);
    pc.camUp    = glm::vec4(camUp, 0.0f);

    // Alpha pass
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_alphaPipeline);
    for (auto& inst : m_activeTrails) {
        if (inst->def->additive || inst->points.size() < 2) continue;
        
        pc.pointCount = static_cast<uint32_t>(inst->points.size());
        pc.headIndex  = 0;
        pc.widthStart = inst->def->widthStart;
        pc.widthEnd   = inst->def->widthEnd;

        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TrailPC), &pc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &inst->descSet, 0, nullptr);
        vkCmdDraw(cmd, (pc.pointCount - 1) * 6, 1, 0, 0);
    }

    // Additive pass
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_additivePipeline);
    for (auto& inst : m_activeTrails) {
        if (!inst->def->additive || inst->points.size() < 2) continue;
        
        pc.pointCount = static_cast<uint32_t>(inst->points.size());
        pc.headIndex  = 0;
        pc.widthStart = inst->def->widthStart;
        pc.widthEnd   = inst->def->widthEnd;

        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TrailPC), &pc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &inst->descSet, 0, nullptr);
        vkCmdDraw(cmd, (pc.pointCount - 1) * 6, 1, 0, 0);
    }
}

void TrailRenderer::createDescriptorLayout() {
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutCI.bindingCount = 2;
    layoutCI.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device.getDevice(), &layoutCI, nullptr, &m_descLayout), "Create Trail Layout");
}

void TrailRenderer::createPipelines() {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::mat4) + sizeof(glm::vec4)*2 + sizeof(uint32_t)*2 + sizeof(float)*2;

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pushConstantRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout), "Create Trail Pipe Layout");

    auto vertCode = readFile(std::string(SHADER_DIR) + "trail_ribbon.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "trail_ribbon.frag.spv");

    VkShaderModule vertMod = createShaderModule(vertCode);
    VkShaderModule fragMod = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo viCI{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo iaCI{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iaCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpCI{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpCI.viewportCount = 1; vpCI.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rsCI{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsCI.polygonMode = VK_POLYGON_MODE_FILL; rsCI.lineWidth = 1.0f; rsCI.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo msCI{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsCI{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dsCI.depthTestEnable = VK_TRUE; dsCI.depthWriteEnable = VK_FALSE; dsCI.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.colorWriteMask = 0xF;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbCI.attachmentCount = 1;
    cbCI.pAttachments = &blend;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynCI{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynCI.dynamicStateCount = 2; dynCI.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipeCI{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipeCI.stageCount = 2;
    pipeCI.pStages = stages;
    pipeCI.pVertexInputState = &viCI;
    pipeCI.pInputAssemblyState = &iaCI;
    pipeCI.pViewportState = &vpCI;
    pipeCI.pRasterizationState = &rsCI;
    pipeCI.pMultisampleState = &msCI;
    pipeCI.pDepthStencilState = &dsCI;
    pipeCI.pColorBlendState = &cbCI;
    pipeCI.pDynamicState = &dynCI;
    pipeCI.layout = m_pipelineLayout;
    pipeCI.renderPass = m_renderPass;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_alphaPipeline), "Alpha Trail Pipe");

    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; // Additive
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_additivePipeline), "Additive Trail Pipe");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
}

VkShaderModule TrailRenderer::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(m_device.getDevice(), &ci, nullptr, &module),
             "Failed to create shader module");
    return module;
}

} // namespace glory
