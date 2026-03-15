#include "renderer/SpriteEffectRenderer.h"
#include "renderer/Device.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <algorithm>
#include <cstring>

namespace glory {

struct SpriteVertex {
    glm::vec3 pos;
    glm::vec2 uv;
};

// ─── Lifecycle ──────────────────────────────────────────────────────────────

SpriteEffectRenderer::~SpriteEffectRenderer() { destroy(); }

void SpriteEffectRenderer::init(const Device& device, VkRenderPass renderPass) {
    m_device = &device;
    createDescriptorResources();
    createPipelines(renderPass);
    createVertexBuffer();
    spdlog::info("SpriteEffectRenderer initialized");
}

void SpriteEffectRenderer::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_alphaPipeline)    vkDestroyPipeline(dev, m_alphaPipeline, nullptr);
    if (m_additivePipeline) vkDestroyPipeline(dev, m_additivePipeline, nullptr);
    if (m_pipelineLayout)   vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_descPool)         vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descLayout)       vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);

    m_alphaPipeline = m_additivePipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_descPool = VK_NULL_HANDLE;
    m_descLayout = VK_NULL_HANDLE;
    m_effects.clear();
    m_active.clear();
    m_device = nullptr;
}

// ─── Descriptor resources ───────────────────────────────────────────────────

void SpriteEffectRenderer::createDescriptorResources() {
    VkDevice dev = m_device->getDevice();

    // Layout: one combined image sampler at binding 0
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings    = &binding;
    vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout);

    // Pool: enough for 16 effect types
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 16;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 16;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool);
}

VkDescriptorSet SpriteEffectRenderer::allocateDescSetForTexture(const Texture& tex) {
    VkDevice dev = m_device->getDevice();

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(dev, &allocInfo, &set);

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView   = tex.getImageView();
    imgInfo.sampler     = tex.getSampler();

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    return set;
}

// ─── Pipeline creation ──────────────────────────────────────────────────────

static std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) {
        spdlog::error("SpriteEffectRenderer: failed to open shader: {}", path);
        return {};
    }
    size_t sz = (size_t)f.tellg();
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), sz);
    return buf;
}

void SpriteEffectRenderer::createPipelines(VkRenderPass renderPass) {
    VkDevice dev = m_device->getDevice();

    // Push constant range
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout);

    // Shader modules — billboard vertex shader for camera-facing sprites
    auto vcode = readFile(std::string(SHADER_DIR) + "sprite_effect_billboard.vert.spv");
    auto fAlpha    = readFile(std::string(SHADER_DIR) + "click_indicator.frag.spv");
    auto fAdditive = readFile(std::string(SHADER_DIR) + "sprite_effect_additive.frag.spv");

    auto createModule = [&](const std::vector<char>& code) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size();
        ci.pCode    = (const uint32_t*)code.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &ci, nullptr, &mod);
        return mod;
    };

    VkShaderModule vMod      = createModule(vcode);
    VkShaderModule fAlphaMod = createModule(fAlpha);
    VkShaderModule fAddMod   = createModule(fAdditive);

    // Shared pipeline state
    VkVertexInputBindingDescription binding{0, sizeof(SpriteVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SpriteVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SpriteVertex, uv)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth   = 1.0f;
    rs.cullMode    = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates    = dyns;

    // ── Alpha-blend pipeline ─────────────────────────────────────────────
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vMod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fAlphaMod;
        stages[1].pName  = "main";

        VkPipelineColorBlendAttachmentState cb{};
        cb.blendEnable         = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.colorBlendOp        = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb.alphaBlendOp        = VK_BLEND_OP_ADD;
        cb.colorWriteMask      = 0xF;

        VkPipelineColorBlendAttachmentState spriteAlphaBlends[2] = {cb, {}};
        VkPipelineColorBlendStateCreateInfo cbCI{};
        cbCI.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbCI.attachmentCount = 2;
        cbCI.pAttachments    = spriteAlphaBlends;

        VkGraphicsPipelineCreateInfo gpCI{};
        gpCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpCI.stageCount          = 2;
        gpCI.pStages             = stages;
        gpCI.pVertexInputState   = &vi;
        gpCI.pInputAssemblyState = &ia;
        gpCI.pViewportState      = &vp;
        gpCI.pRasterizationState = &rs;
        gpCI.pMultisampleState   = &ms;
        gpCI.pDepthStencilState  = &ds;
        gpCI.pColorBlendState    = &cbCI;
        gpCI.pDynamicState       = &dy;
        gpCI.layout              = m_pipelineLayout;
        gpCI.renderPass          = renderPass;
        vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &m_alphaPipeline);
    }

    // ── Additive-blend pipeline ──────────────────────────────────────────
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vMod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fAddMod;
        stages[1].pName  = "main";

        VkPipelineColorBlendAttachmentState cb{};
        cb.blendEnable         = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.colorBlendOp        = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.alphaBlendOp        = VK_BLEND_OP_ADD;
        cb.colorWriteMask      = 0xF;

        VkPipelineColorBlendAttachmentState spriteAddBlends[2] = {cb, {}};
        VkPipelineColorBlendStateCreateInfo cbCI{};
        cbCI.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbCI.attachmentCount = 2;
        cbCI.pAttachments    = spriteAddBlends;

        VkGraphicsPipelineCreateInfo gpCI{};
        gpCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpCI.stageCount          = 2;
        gpCI.pStages             = stages;
        gpCI.pVertexInputState   = &vi;
        gpCI.pInputAssemblyState = &ia;
        gpCI.pViewportState      = &vp;
        gpCI.pRasterizationState = &rs;
        gpCI.pMultisampleState   = &ms;
        gpCI.pDepthStencilState  = &ds;
        gpCI.pColorBlendState    = &cbCI;
        gpCI.pDynamicState       = &dy;
        gpCI.layout              = m_pipelineLayout;
        gpCI.renderPass          = renderPass;
        vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &m_additivePipeline);
    }

    vkDestroyShaderModule(dev, fAddMod, nullptr);
    vkDestroyShaderModule(dev, fAlphaMod, nullptr);
    vkDestroyShaderModule(dev, vMod, nullptr);
}

// ─── Vertex buffer (ground-plane billboard quad) ────────────────────────────

void SpriteEffectRenderer::createVertexBuffer() {
    // Billboard quad: XY offsets (camera-facing), UV maps to atlas cell
    SpriteVertex vertices[6] = {
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},  // bottom-left
        {{ 1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},  // bottom-right
        {{ 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f}},  // top-right
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},  // bottom-left
        {{ 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f}},  // top-right
        {{-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f}},  // top-left
    };
    VkDeviceSize size = sizeof(vertices);
    m_vertexBuffer = Buffer(m_device->getAllocator(), size,
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            VMA_MEMORY_USAGE_CPU_TO_GPU);
    std::memcpy(m_vertexBuffer.map(), vertices, (size_t)size);
}

// ─── Effect registration ────────────────────────────────────────────────────

uint32_t SpriteEffectRenderer::registerEffect(const std::string& name,
                                              const std::string& atlasPath,
                                              int gridCount, int frameCount,
                                              float duration, bool additive) {
    EffectType fx;
    fx.name       = name;
    fx.gridCount  = gridCount;
    fx.frameCount = frameCount;
    fx.duration   = duration;
    fx.additive   = additive;
    fx.atlas      = std::make_unique<Texture>(*m_device, atlasPath);
    fx.descSet    = allocateDescSetForTexture(*fx.atlas);

    uint32_t id = static_cast<uint32_t>(m_effects.size());
    m_effects.push_back(std::move(fx));
    spdlog::info("SpriteEffectRenderer: registered '{}' (id={}, {}×{} grid, {} frames, {:.2f}s, {})",
                 name, id, gridCount, gridCount, frameCount, duration,
                 additive ? "additive" : "alpha");
    return id;
}

// ─── Spawn / Update / Render ────────────────────────────────────────────────

void SpriteEffectRenderer::spawn(uint32_t effectId, const glm::vec3& position,
                                 float size, const glm::vec4& tint) {
    if (effectId >= m_effects.size()) return;
    if (static_cast<int>(m_active.size()) >= MAX_INSTANCES) {
        m_active.erase(m_active.begin()); // evict oldest
    }
    Instance inst;
    inst.effectId = effectId;
    inst.position = position;
    inst.size     = size;
    inst.tint     = tint;
    inst.duration = m_effects[effectId].duration;
    m_active.push_back(inst);
}

void SpriteEffectRenderer::update(float dt) {
    for (auto& inst : m_active)
        inst.elapsed += dt;

    // Remove expired
    m_active.erase(
        std::remove_if(m_active.begin(), m_active.end(),
                       [](const Instance& i) { return i.elapsed >= i.duration; }),
        m_active.end());
}

void SpriteEffectRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                  const glm::vec3& /*cameraPos*/) {
    if (m_active.empty()) return;

    VkDeviceSize offsets[] = {0};
    VkBuffer buf = m_vertexBuffer.getBuffer();

    // Group draws by pipeline (alpha vs additive) to minimize pipeline switches
    for (int pass = 0; pass < 2; ++pass) {
        bool wantAdditive = (pass == 1);
        VkPipeline pipeline = wantAdditive ? m_additivePipeline : m_alphaPipeline;
        bool bound = false;

        for (const auto& inst : m_active) {
            const auto& fx = m_effects[inst.effectId];
            if (fx.additive != wantAdditive) continue;

            if (!bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdBindVertexBuffers(cmd, 0, 1, &buf, offsets);
                bound = true;
            }

            // Bind this effect's atlas texture
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_pipelineLayout, 0, 1, &fx.descSet, 0, nullptr);

            // Compute frame index from elapsed time
            float t = inst.elapsed / inst.duration;
            int frameIndex = static_cast<int>(t * static_cast<float>(fx.frameCount));
            if (frameIndex >= fx.frameCount) frameIndex = fx.frameCount - 1;

            PushConstants pc{};
            pc.viewProj   = viewProj;
            pc.center     = inst.position;
            pc.size       = inst.size;
            pc.frameIndex = frameIndex;
            pc.gridCount  = fx.gridCount;
            pc.tint       = inst.tint;

            vkCmdPushConstants(cmd, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);

            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
    }
}

} // namespace glory
