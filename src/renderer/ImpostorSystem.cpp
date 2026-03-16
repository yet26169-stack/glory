#include "renderer/ImpostorSystem.h"
#include "renderer/Device.h"
#include "renderer/RenderFormats.h"

#include <spdlog/spdlog.h>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace glory {

// ── Init / Cleanup ─────────────────────────────────────────────────────────

void ImpostorSystem::init(const Device& device, const RenderFormats& formats) {
    m_device = &device;

    // Placeholder atlas: solid white 2048×2048 RGBA
    {
        uint32_t w = ATLAS_SIZE, h = ATLAS_SIZE;
        std::vector<uint32_t> pixels(w * h, 0xFFFFFFFF); // RGBA white
        m_atlas = Texture::createFromPixels(device, pixels.data(), w, h,
                                            VK_FORMAT_R8G8B8A8_UNORM);
    }

    createPipeline(formats);
    m_initialized = true;
    spdlog::info("[ImpostorSystem] initialised (atlas {}x{}, cell {}px)",
                 ATLAS_SIZE, ATLAS_SIZE, CELL_SIZE);
}

void ImpostorSystem::cleanup() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_pipeline)       vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_dsLayout)       vkDestroyDescriptorSetLayout(dev, m_dsLayout, nullptr);
    if (m_descPool)       vkDestroyDescriptorPool(dev, m_descPool, nullptr);

    m_instanceBuffer.destroy();
    m_atlas = Texture{}; // RAII cleanup via move-assignment

    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_dsLayout = VK_NULL_HANDLE;
    m_descPool = VK_NULL_HANDLE;
    m_initialized = false;
}

// ── Registration ───────────────────────────────────────────────────────────

void ImpostorSystem::registerUnitType(const std::string& modelName,
                                       float worldWidth, float worldHeight,
                                       uint32_t angleCount) {
    if (m_entries.count(modelName)) return; // already registered

    ImpostorEntry entry;
    entry.angleCount  = angleCount;
    entry.worldWidth  = worldWidth;
    entry.worldHeight = worldHeight;

    // Allocate cells in the atlas grid (one cell per angle)
    entry.startCol = m_nextCell % CELLS_PER_ROW;
    entry.startRow = m_nextCell / CELLS_PER_ROW;
    entry.atlasIndex = 0; // single atlas page for now

    m_nextCell += angleCount;
    m_entries[modelName] = entry;

    spdlog::debug("[ImpostorSystem] registered '{}' — {} angles, cell ({},{})",
                  modelName, angleCount, entry.startCol, entry.startRow);
}

void ImpostorSystem::generateAtlas() {
    // TODO: off-screen render each unit type from `angleCount` evenly spaced
    // camera angles into the atlas texture.  For now the atlas remains the
    // solid white placeholder — entities will appear as tinted white quads,
    // which is correct behaviour for testing the billboard geometry pipeline.

    spdlog::info("[ImpostorSystem] atlas generated (placeholder) — {} unit types, {} cells used",
                 m_entries.size(), m_nextCell);
}

const ImpostorEntry* ImpostorSystem::getEntry(const std::string& modelName) const {
    auto it = m_entries.find(modelName);
    return (it != m_entries.end()) ? &it->second : nullptr;
}

// ── Per-frame instance building ────────────────────────────────────────────

ImpostorInstance ImpostorSystem::buildInstance(const std::string& modelName,
                                               glm::vec3 worldPos,
                                               glm::vec3 cameraPos,
                                               glm::vec4 tint) const {
    ImpostorInstance inst{};
    inst.worldPos = worldPos;
    inst.tint     = tint;
    inst.scale    = 1.0f;

    auto it = m_entries.find(modelName);
    if (it == m_entries.end()) {
        inst.uvRect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        return inst;
    }

    const auto& e = it->second;
    inst.scale = e.worldHeight;

    // Determine angle from camera to entity (XZ plane)
    glm::vec2 dir(worldPos.x - cameraPos.x, worldPos.z - cameraPos.z);
    float angle = angleFromDirection(dir);
    uint32_t angleIdx = bestAngleIndex(angle, e.angleCount);

    // Cell index in the atlas grid
    uint32_t cellIdx = (e.startRow * CELLS_PER_ROW + e.startCol) + angleIdx;
    inst.uvRect = cellUVRect(cellIdx);

    return inst;
}

void ImpostorSystem::beginFrame() {
    m_frameInstances.clear();
}

void ImpostorSystem::addInstance(const ImpostorInstance& inst) {
    m_frameInstances.push_back(inst);
}

// ── Render ─────────────────────────────────────────────────────────────────

void ImpostorSystem::render(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (!m_initialized || m_frameInstances.empty()) return;

    ensureInstanceCapacity(m_frameInstances.size());

    std::memcpy(m_instanceBuffer.map(), m_frameInstances.data(),
                m_frameInstances.size() * sizeof(ImpostorInstance));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(glm::mat4), &viewProj);

    VkBuffer bufs[] = { m_instanceBuffer.getBuffer() };
    VkDeviceSize offs[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, bufs, offs);

    // 6 vertices per quad (2 triangles), instanced
    vkCmdDraw(cmd, 6, static_cast<uint32_t>(m_frameInstances.size()), 0, 0);
}

// ── Pipeline creation ──────────────────────────────────────────────────────

void ImpostorSystem::createPipeline(const RenderFormats& formats) {
    VkDevice dev = m_device->getDevice();

    // Descriptor set layout: binding 0 = atlas sampler
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding         = 0;
    samplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dsLayoutCI{};
    dsLayoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsLayoutCI.bindingCount = 1;
    dsLayoutCI.pBindings    = &samplerBinding;
    vkCreateDescriptorSetLayout(dev, &dsLayoutCI, nullptr, &m_dsLayout);

    // Descriptor pool + set
    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool);

    VkDescriptorSetAllocateInfo allocI{};
    allocI.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocI.descriptorPool     = m_descPool;
    allocI.descriptorSetCount = 1;
    allocI.pSetLayouts        = &m_dsLayout;
    vkAllocateDescriptorSets(dev, &allocI, &m_descSet);

    // Write atlas texture to descriptor
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView   = m_atlas.getImageView();
    imgInfo.sampler     = m_atlas.getSampler();

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_descSet;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    // Push constant: mat4 viewProj
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_dsLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout);

    // Vertex input: per-instance ImpostorInstance
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(ImpostorInstance);
    binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attrs[4]{};
    // location 0: worldPos (vec3)
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                 static_cast<uint32_t>(offsetof(ImpostorInstance, worldPos)) };
    // location 1: scale (float)
    attrs[1] = { 1, 0, VK_FORMAT_R32_SFLOAT,
                 static_cast<uint32_t>(offsetof(ImpostorInstance, scale)) };
    // location 2: uvRect (vec4)
    attrs[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                 static_cast<uint32_t>(offsetof(ImpostorInstance, uvRect)) };
    // location 3: tint (vec4)
    attrs[3] = { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                 static_cast<uint32_t>(offsetof(ImpostorInstance, tint)) };

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = 4;
    vertexInput.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE; // billboards face camera
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.blendEnable         = VK_TRUE;
    blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.colorBlendOp        = VK_BLEND_OP_ADD;
    blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendAtt.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blendAtt;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dynStates;

    // Load shaders (impostor_billboard.vert / .frag)
    auto loadModule = [&](const std::string& path) -> VkShaderModule {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            spdlog::warn("[ImpostorSystem] shader not found: {}", path);
            return VK_NULL_HANDLE;
        }
        size_t sz = static_cast<size_t>(file.tellg());
        std::vector<char> code(sz);
        file.seekg(0);
        file.read(code.data(), static_cast<std::streamsize>(sz));
        VkShaderModuleCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = sz;
        ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &ci, nullptr, &mod);
        return mod;
    };

    VkShaderModule vertMod = loadModule("shaders/impostor_billboard.vert.spv");
    VkShaderModule fragMod = loadModule("shaders/impostor_billboard.frag.spv");

    if (!vertMod || !fragMod) {
        spdlog::warn("[ImpostorSystem] shaders missing — pipeline not created");
        if (vertMod) vkDestroyShaderModule(dev, vertMod, nullptr);
        if (fragMod) vkDestroyShaderModule(dev, fragMod, nullptr);
        return;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    // Dynamic rendering (VK_KHR_dynamic_rendering)
    VkPipelineRenderingCreateInfo renderingCI = formats.pipelineRenderingCI();

    VkGraphicsPipelineCreateInfo pipeCI{};
    pipeCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeCI.pNext               = &renderingCI;
    pipeCI.stageCount          = 2;
    pipeCI.pStages             = stages;
    pipeCI.pVertexInputState   = &vertexInput;
    pipeCI.pInputAssemblyState = &inputAssembly;
    pipeCI.pViewportState      = &viewportState;
    pipeCI.pRasterizationState = &raster;
    pipeCI.pMultisampleState   = &ms;
    pipeCI.pDepthStencilState  = &depthStencil;
    pipeCI.pColorBlendState    = &blend;
    pipeCI.pDynamicState       = &dynState;
    pipeCI.layout              = m_pipelineLayout;

    vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_pipeline);

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
}

void ImpostorSystem::ensureInstanceCapacity(size_t needed) {
    if (needed <= m_instanceCapacity) return;

    size_t newCap = std::max(needed, INITIAL_INSTANCE_CAP);
    if (m_instanceCapacity > 0) {
        m_instanceBuffer.destroy();
    }
    m_instanceBuffer = Buffer(m_device->getAllocator(),
        static_cast<VkDeviceSize>(newCap * sizeof(ImpostorInstance)),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_instanceCapacity = newCap;
}

// ── Angle math ─────────────────────────────────────────────────────────────

float ImpostorSystem::angleFromDirection(glm::vec2 dir) const {
    if (glm::length(dir) < 0.001f) return 0.0f;
    return std::atan2(dir.y, dir.x); // radians, [-π, π]
}

uint32_t ImpostorSystem::bestAngleIndex(float angleRad, uint32_t angleCount) const {
    // Map [-π, π] to [0, 2π]
    float a = angleRad;
    if (a < 0.0f) a += glm::two_pi<float>();

    float step = glm::two_pi<float>() / static_cast<float>(angleCount);
    uint32_t idx = static_cast<uint32_t>(std::round(a / step)) % angleCount;
    return idx;
}

glm::vec4 ImpostorSystem::cellUVRect(uint32_t cellIndex) const {
    uint32_t col = cellIndex % CELLS_PER_ROW;
    uint32_t row = cellIndex / CELLS_PER_ROW;

    float invSize = 1.0f / static_cast<float>(ATLAS_SIZE);
    float u0 = static_cast<float>(col * CELL_SIZE) * invSize;
    float v0 = static_cast<float>(row * CELL_SIZE) * invSize;
    float u1 = u0 + static_cast<float>(CELL_SIZE) * invSize;
    float v1 = v0 + static_cast<float>(CELL_SIZE) * invSize;

    return glm::vec4(u0, v0, u1, v1);
}

} // namespace glory
