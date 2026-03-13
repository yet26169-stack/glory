#include "renderer/GroundDecalRenderer.h"
#include "renderer/Model.h"
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

GroundDecalRenderer::GroundDecalRenderer(const Device& device, VkRenderPass renderPass)
    : m_device(device), m_renderPass(renderPass) 
{
    m_defaultTexture = std::make_unique<Texture>(Texture::createDefault(device));
    m_quadMesh = std::make_unique<Model>(Model::createUnitQuad(device, device.getAllocator()));

    createDescriptorLayout();
    createPipelines();

    // Pool for decal descriptor sets (one per unique texture)
    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32 };
    VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolCI.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets = 32;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(device.getDevice(), &poolCI, nullptr, &m_descPool), "Create Decal pool");
}

GroundDecalRenderer::~GroundDecalRenderer() {
    VkDevice dev = m_device.getDevice();
    vkDeviceWaitIdle(dev);
    
    if (m_alphaPipeline) vkDestroyPipeline(dev, m_alphaPipeline, nullptr);
    if (m_additivePipeline) vkDestroyPipeline(dev, m_additivePipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
}

void GroundDecalRenderer::registerDecal(const DecalDef& def) {
    m_defs[def.id] = def;
}

uint32_t GroundDecalRenderer::spawn(const std::string& decalDefId, glm::vec3 center, float radius, float rotation) {
    auto it = m_defs.find(decalDefId);
    if (it == m_defs.end()) {
        spdlog::warn("GroundDecalRenderer: unknown def '{}'", decalDefId);
        return 0;
    }

    DecalInstance inst;
    inst.handle = m_nextHandle++;
    inst.def = &it->second;
    inst.center = center;
    inst.radius = radius;
    inst.rotation = rotation;
    inst.texture = getOrLoadTexture(inst.def->texturePath);

    m_activeDecals.push_back(inst);
    return inst.handle;
}

void GroundDecalRenderer::update(float dt) {
    for (auto it = m_activeDecals.begin(); it != m_activeDecals.end(); ) {
        it->elapsed += dt;
        it->rotation += it->def->rotationSpeed * dt;

        if (it->elapsed >= it->def->duration) {
            it = m_activeDecals.erase(it);
        } else {
            ++it;
        }
    }
}

void GroundDecalRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj, float appTime) {
    if (m_activeDecals.empty()) return;

    for (const auto& inst : m_activeDecals) {
        float alpha = 1.0f;
        if (inst.elapsed < inst.def->fadeInTime) {
            alpha = inst.elapsed / inst.def->fadeInTime;
        } else if (inst.elapsed > (inst.def->duration - inst.def->fadeOutTime)) {
            alpha = (inst.def->duration - inst.elapsed) / inst.def->fadeOutTime;
        }
        alpha = std::clamp(alpha, 0.0f, 1.0f);

        DecalPC pc;
        pc.viewProj = viewProj;
        pc.center = inst.center;
        pc.radius = inst.radius;
        pc.rotation = inst.rotation;
        pc.alpha = alpha;
        pc.elapsed = inst.elapsed;
        pc.appTime = appTime;
        pc.color = inst.def->color;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inst.def->additive ? m_additivePipeline : m_alphaPipeline);
        
        VkDescriptorSet ds = getOrCreateDescriptorSet(inst.texture);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &ds, 0, nullptr);
        
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DecalPC), &pc);
        
        m_quadMesh->draw(cmd);
    }
}

void GroundDecalRenderer::destroy(uint32_t handle) {
    m_activeDecals.erase(std::remove_if(m_activeDecals.begin(), m_activeDecals.end(),
        [handle](const DecalInstance& d) { return d.handle == handle; }), m_activeDecals.end());
}

void GroundDecalRenderer::destroyAll() {
    m_activeDecals.clear();
}

void GroundDecalRenderer::createDescriptorLayout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutCI.bindingCount = 1;
    layoutCI.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device.getDevice(), &layoutCI, nullptr, &m_descLayout), "Create Decal Layout");
}

void GroundDecalRenderer::createPipelines() {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(DecalPC);

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout), "Create Decal Pipe Layout");

    auto vertCode = readFile(std::string(SHADER_DIR) + "ground_decal.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "ground_decal.frag.spv");

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

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_alphaPipeline), "Alpha Decal Pipe");

    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; // Additive
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_additivePipeline), "Additive Decal Pipe");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
}

VkShaderModule GroundDecalRenderer::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(m_device.getDevice(), &ci, nullptr, &module),
             "Failed to create shader module");
    return module;
}

Texture* GroundDecalRenderer::getOrLoadTexture(const std::string& path) {
    if (path.empty()) return m_defaultTexture.get();
    auto it = m_textureCache.find(path);
    if (it != m_textureCache.end()) return it->second.get();

    try {
        auto tex = std::make_unique<Texture>(m_device, path);
        Texture* ptr = tex.get();
        m_textureCache[path] = std::move(tex);
        return ptr;
    } catch (...) {
        spdlog::warn("Failed to load decal texture: {}", path);
        return m_defaultTexture.get();
    }
}

VkDescriptorSet GroundDecalRenderer::getOrCreateDescriptorSet(Texture* tex) {
    uint64_t key = (uint64_t)tex->getImageView();
    // Use texture image view handle as key for descriptor set
    // Simplified for now: one set per unique texture view
    static std::unordered_map<uint64_t, VkDescriptorSet> cachedSets;
    
    if (cachedSets.count(key)) return cachedSets[key];

    VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descLayout;
    
    VkDescriptorSet ds;
    VK_CHECK(vkAllocateDescriptorSets(m_device.getDevice(), &allocInfo, &ds), "Alloc Decal Desc Set");

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView = tex->getImageView();
    imgInfo.sampler = tex->getSampler();

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = ds;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
    cachedSets[key] = ds;
    return ds;
}

} // namespace glory
