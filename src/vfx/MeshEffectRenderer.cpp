#include "vfx/MeshEffectRenderer.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <algorithm>

namespace glory {

static std::vector<char> readFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Failed to open file: " + filepath);
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    return buffer;
}

MeshEffectRenderer::MeshEffectRenderer(const Device& device, VkRenderPass renderPass)
    : m_device(device), m_renderPass(renderPass) {}

MeshEffectRenderer::~MeshEffectRenderer() {
    VkDevice dev = m_device.getDevice();
    vkDeviceWaitIdle(dev);
    for (auto& pair : m_pipelines) {
        vkDestroyPipeline(dev, pair.second.pipeline, nullptr);
        vkDestroyPipelineLayout(dev, pair.second.layout, nullptr);
    }
}

void MeshEffectRenderer::registerDef(const MeshEffectDef& def) {
    m_defs[def.id] = def;
}

void MeshEffectRenderer::spawn(const std::string& defId, glm::vec3 position,
                               glm::vec3 direction, float scale) {
    auto it = m_defs.find(defId);
    if (it == m_defs.end()) {
        spdlog::warn("MeshEffectRenderer: unknown def '{}'", defId);
        return;
    }

    const auto& def = it->second;
    MeshEffectInstance inst;
    inst.handle = m_nextHandle++;
    inst.def = &def;
    inst.position = position;
    inst.direction = direction;
    inst.scaleBase = scale;
    
    // Auto-align rotation to direction if it's not zero
    if (glm::length(direction) > 0.001f) {
        inst.currentRotation = std::atan2(direction.x, direction.z);
    }

    m_active.push_back(inst);
}

void MeshEffectRenderer::update(float dt) {
    for (auto it = m_active.begin(); it != m_active.end(); ) {
        it->elapsed += dt;
        it->currentRotation += it->def->rotationSpeed * dt;

        if (it->elapsed >= it->def->duration) {
            it = m_active.erase(it);
        } else {
            ++it;
        }
    }
}

void MeshEffectRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj, float appTime) {
    for (const auto& inst : m_active) {
        const auto& def = *inst.def;
        PipelineKey key{ def.vertShader, def.fragShader, def.additive };
        if (m_pipelines.find(key) == m_pipelines.end()) {
            createPipeline(key);
        }
        
        auto& ps = m_pipelines[key];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ps.pipeline);

        float t = inst.elapsed / def.duration;
        float scale = glm::mix(def.scaleStart, def.scaleEnd, t) * inst.scaleBase;
        float alpha = glm::mix(def.alphaStart, def.alphaEnd, t);
        glm::vec4 color = glm::mix(def.colorStart, def.colorEnd, t);

        MeshEffectPC pc;
        pc.viewProj = viewProj;
        pc.color = color;
        pc.data = { alpha, inst.elapsed, appTime, scale };
        pc.posRot = { inst.position, inst.currentRotation };
        pc.dir = { inst.direction, 0.0f };

        vkCmdPushConstants(cmd, ps.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshEffectPC), &pc);

        MeshData* md = getOrLoadMesh(def.meshPath);
        if (md && md->model) {
            md->model->draw(cmd);
        }
    }
}

void MeshEffectRenderer::createPipeline(const PipelineKey& key) {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.size = sizeof(MeshEffectPC);

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pcRange;
    
    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &layout), "Create MeshEffect Pipe Layout");

    auto vertCode = readFile(std::string(SHADER_DIR) + key.vert);
    auto fragCode = readFile(std::string(SHADER_DIR) + key.frag);

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

    auto vertBinding = Vertex::getBindingDescription();
    auto vertAttrs   = Vertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo viCI{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    viCI.vertexBindingDescriptionCount = 1;
    viCI.pVertexBindingDescriptions = &vertBinding;
    viCI.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertAttrs.size());
    viCI.pVertexAttributeDescriptions = vertAttrs.data();

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
    blend.dstColorBlendFactor = key.additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendAttachmentState meshFxBlends[2] = {blend, {}};
    VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbCI.attachmentCount = 2;
    cbCI.pAttachments = meshFxBlends;

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
    pipeCI.layout = layout;
    pipeCI.renderPass = m_renderPass;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &pipeline), "Create MeshEffect Pipe");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);

    m_pipelines[key] = { layout, pipeline };
}

MeshEffectRenderer::MeshData* MeshEffectRenderer::getOrLoadMesh(const std::string& path) {
    if (path.empty()) return nullptr;
    if (m_meshes.count(path)) return &m_meshes[path];

    try {
        auto model = std::make_unique<Model>(Model::loadFromGLB(m_device, m_device.getAllocator(), path));
        m_meshes[path] = { std::move(model) };
        return &m_meshes[path];
    } catch (...) {
        spdlog::error("Failed to load VFX mesh: {}", path);
        return nullptr;
    }
}

VkShaderModule MeshEffectRenderer::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(m_device.getDevice(), &ci, nullptr, &module), "Failed shader module");
    return module;
}

} // namespace glory
