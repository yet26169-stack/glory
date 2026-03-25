#include "renderer/ImpostorSystem.h"
#include "renderer/Device.h"
#include "renderer/RenderFormats.h"
#include "renderer/Model.h"
#include "scene/Scene.h"

#include <spdlog/spdlog.h>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace glory {

struct CapturePC {
    glm::mat4 viewProj;
    glm::mat4 model;
};

// ── Init / Cleanup ─────────────────────────────────────────────────────────

void ImpostorSystem::init(const Device& device, const RenderFormats& formats) {
    m_device = &device;

    // Create atlas texture with COLOR_ATTACHMENT + SAMPLED usage
    m_atlas = Texture::createRenderable(device, ATLAS_SIZE, ATLAS_SIZE,
                                        VK_FORMAT_R8G8B8A8_UNORM);

    createPipeline(formats);
    createCapturePipeline();
    m_initialized = true;
    spdlog::info("[ImpostorSystem] initialised (atlas {}x{}, cell {}px)",
                 ATLAS_SIZE, ATLAS_SIZE, CELL_SIZE);
}

void ImpostorSystem::cleanup() {
    if (!m_initialized) return;
    VkDevice dev = m_device->getDevice();

    if (m_pipeline)       vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_dsLayout)       vkDestroyDescriptorSetLayout(dev, m_dsLayout, nullptr);
    if (m_descPool)       vkDestroyDescriptorPool(dev, m_descPool, nullptr);

    m_instanceBuffer.destroy();
    m_atlas = Texture{}; // RAII cleanup via move-assignment

    if (m_capturePipeline) vkDestroyPipeline(dev, m_capturePipeline, nullptr);
    if (m_captureLayout)   vkDestroyPipelineLayout(dev, m_captureLayout, nullptr);

    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_dsLayout = VK_NULL_HANDLE;
    m_descPool = VK_NULL_HANDLE;
    m_capturePipeline = VK_NULL_HANDLE;
    m_captureLayout = VK_NULL_HANDLE;
    m_initialized = false;
}

// ── Registration ───────────────────────────────────────────────────────────

void ImpostorSystem::registerUnitType(const std::string& modelName,
                                       uint32_t meshIndex,
                                       float worldWidth, float worldHeight,
                                       uint32_t angleCount) {
    if (m_entries.count(modelName)) return; // already registered

    ImpostorEntry entry;
    entry.meshIndex   = meshIndex;
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

void ImpostorSystem::generateAtlas(const Scene& scene) {
    if (!m_device || m_entries.empty()) return;

    VkDevice dev = m_device->getDevice();
    auto poolLock = m_device->lockGraphicsPool();
    VkCommandPool pool = m_device->getGraphicsCommandPool();

    // 1. Create depth buffer for capture
    VkFormat depthFmt = m_device->findDepthFormat();
    Image depthBuffer(*m_device, ATLAS_SIZE, ATLAS_SIZE, depthFmt,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT);

    // 2. Transition atlas to COLOR_ATTACHMENT_OPTIMAL
    auto transition = [&](VkCommandBuffer cmd, VkImage img, VkImageLayout oldL, VkImageLayout newL, VkImageAspectFlags aspect) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        
        if (newL == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            b.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask = VK_ACCESS_2_NONE;
            b.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        } else if (newL == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            b.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        } else if (newL == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            b.srcStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
            b.srcAccessMask = VK_ACCESS_2_NONE;
            b.dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }

        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &ai, &cmd);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    transition(cmd, m_atlas.getImage(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transition(cmd, depthBuffer.getImage(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo colorAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAtt.imageView = m_atlas.getImageView();
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.clearValue.color = {{0,0,0,0}};

    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = depthBuffer.getImageView();
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea = {{0,0}, {ATLAS_SIZE, ATLAS_SIZE}};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments = &colorAtt;
    renderInfo.pDepthAttachment = &depthAtt;

    vkCmdBeginRendering(cmd, &renderInfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_capturePipeline);

    for (auto& [name, entry] : m_entries) {
        if (entry.meshIndex >= scene.getMeshes().size()) continue;
        const Model& model = scene.getMesh(entry.meshIndex);

        // Orthographic projection for capturing quads
        float h = entry.worldHeight * 0.6f;
        float w = h; 
        glm::mat4 proj = glm::ortho(-w, w, 0.0f, entry.worldHeight * 1.2f, -10.0f, 10.0f);
        proj[1][1] *= -1; // Vulkan Y flip

        for (uint32_t a = 0; entry.angleCount > 0 && a < entry.angleCount; ++a) {
            float angle = (static_cast<float>(a) / entry.angleCount) * glm::two_pi<float>();
            
            // View matrix: orbit around origin
            float camDist = 5.0f;
            glm::vec3 camPos = { std::cos(angle) * camDist, entry.worldHeight * 0.5f, std::sin(angle) * camDist };
            glm::mat4 view = glm::lookAt(camPos, {0, entry.worldHeight * 0.5f, 0}, {0, 1, 0});

            CapturePC pc;
            pc.viewProj = proj * view;
            pc.model    = glm::mat4(1.0f);

            uint32_t cellIdx = (entry.startRow * CELLS_PER_ROW + entry.startCol) + a;
            uint32_t col = cellIdx % CELLS_PER_ROW;
            uint32_t row = cellIdx / CELLS_PER_ROW;

            VkViewport vp{ (float)col * CELL_SIZE, (float)row * CELL_SIZE, (float)CELL_SIZE, (float)CELL_SIZE, 0, 1 };
            VkRect2D sc{ {(int32_t)(col * CELL_SIZE), (int32_t)(row * CELL_SIZE)}, {CELL_SIZE, CELL_SIZE} };
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);

            vkCmdPushConstants(cmd, m_captureLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(CapturePC), &pc);
            model.draw(cmd);
        }
    }

    vkCmdEndRendering(cmd);
    transition(cmd, m_atlas.getImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    m_device->submitGraphicsLegacy(1, &submit);
    m_device->graphicsQueueWaitIdle();
    vkFreeCommandBuffers(dev, pool, 1, &cmd);

    // Update descriptor set with the now-filled atlas
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView   = m_atlas.getImageView();
    imgInfo.sampler     = m_atlas.getSampler();

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = m_descSet;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    spdlog::info("[ImpostorSystem] atlas generated — {} unit types, {} cells used",
                 m_entries.size(), m_nextCell);
}

const ImpostorEntry* ImpostorSystem::getEntry(const std::string& modelName) const {
    auto it = m_entries.find(modelName);
    if (it == m_entries.end()) return nullptr;
    return &it->second;
}

// ── Per-frame instance data ──────────────────────────────────────────

void ImpostorSystem::beginFrame() {
    m_frameInstances.clear();
}

void ImpostorSystem::addInstance(const ImpostorInstance& inst) {
    m_frameInstances.push_back(inst);
}

ImpostorInstance ImpostorSystem::buildInstance(const std::string& modelName,
                                              glm::vec3 worldPos,
                                              glm::vec3 cameraPos,
                                              glm::vec4 tint) const {
    const ImpostorEntry* entry = getEntry(modelName);
    if (!entry) {
        // Fallback to first available entry if type not found
        if (m_entries.empty()) return { worldPos, 1.0f, glm::vec4(0,0,1,1), tint };
        entry = &m_entries.begin()->second;
    }

    // Direction from entity to camera in XZ plane
    glm::vec2 dir = glm::normalize(glm::vec2(cameraPos.x - worldPos.x, cameraPos.z - worldPos.z));
    float angle = angleFromDirection(dir);
    uint32_t angleIdx = bestAngleIndex(angle, entry->angleCount);

    uint32_t cellIdx = (entry->startRow * CELLS_PER_ROW + entry->startCol) + angleIdx;
    
    ImpostorInstance inst;
    inst.worldPos = worldPos;
    inst.scale    = entry->worldHeight; // billboards sized by unit height
    inst.uvRect   = cellUVRect(cellIdx);
    inst.tint     = tint;
    return inst;
}

// ── Rendering ─────────────────────────────────────────────────────────────

void ImpostorSystem::render(VkCommandBuffer cmd, const glm::mat4& viewProj) {
    if (!m_initialized || m_frameInstances.empty()) return;

    ensureInstanceCapacity(m_frameInstances.size());

    // Upload instance data
    void* mapped = m_instanceBuffer.map();
    std::memcpy(mapped, m_frameInstances.data(),
                m_frameInstances.size() * sizeof(ImpostorInstance));
    m_instanceBuffer.unmap();
    m_instanceBuffer.flush();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(glm::mat4), &viewProj);

    VkDeviceSize offsets[] = { 0 };
    VkBuffer     buf       = m_instanceBuffer.getBuffer();
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, offsets);

    vkCmdDraw(cmd, 6, static_cast<uint32_t>(m_frameInstances.size()), 0, 0);
}

// ── Helpers ──────────────────────────────────────────────────────────────

void ImpostorSystem::createPipeline(const RenderFormats& formats) {
    VkDevice dev = m_device->getDevice();

    // 1. Descriptor Set Layout
    VkDescriptorSetLayoutBinding binding{};
    binding.binding            = 0;
    binding.descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount    = 1;
    binding.stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = 1;
    layoutCI.pBindings    = &binding;
    vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_dsLayout);

    // 2. Pipeline Layout
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo pipeLayoutCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeLayoutCI.setLayoutCount = 1;
    pipeLayoutCI.pSetLayouts    = &m_dsLayout;
    pipeLayoutCI.pushConstantRangeCount = 1;
    pipeLayoutCI.pPushConstantRanges    = &pc;
    vkCreatePipelineLayout(dev, &pipeLayoutCI, nullptr, &m_pipelineLayout);

    // 3. Descriptor Pool & Set
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool);

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_dsLayout;
    vkAllocateDescriptorSets(dev, &allocInfo, &m_descSet);

    // 4. Graphics Pipeline
    auto loadModule = [&](const std::string& path) -> VkShaderModule {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) return VK_NULL_HANDLE;
        size_t sz = static_cast<size_t>(file.tellg());
        std::vector<char> code(sz);
        file.seekg(0);
        file.read(code.data(), static_cast<std::streamsize>(sz));
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = sz;
        ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod;
        vkCreateShaderModule(dev, &ci, nullptr, &mod);
        return mod;
    };

    VkShaderModule vmod = loadModule("shaders/impostor_billboard.vert.spv");
    VkShaderModule fmod = loadModule("shaders/impostor_billboard.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName = "main";

    // Vertex input for billboards (instanced)
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding   = 0;
    bindingDesc.stride    = sizeof(ImpostorInstance);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attrDescs[4]{};
    // inWorldPos
    attrDescs[0].binding  = 0;
    attrDescs[0].location = 0;
    attrDescs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrDescs[0].offset   = offsetof(ImpostorInstance, worldPos);
    // inScale
    attrDescs[1].binding  = 0;
    attrDescs[1].location = 1;
    attrDescs[1].format   = VK_FORMAT_R32_SFLOAT;
    attrDescs[1].offset   = offsetof(ImpostorInstance, scale);
    // inUVRect
    attrDescs[2].binding  = 0;
    attrDescs[2].location = 2;
    attrDescs[2].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrDescs[2].offset   = offsetof(ImpostorInstance, uvRect);
    // inTint
    attrDescs[3].binding  = 0;
    attrDescs[3].location = 3;
    attrDescs[3].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrDescs[3].offset   = offsetof(ImpostorInstance, tint);

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &bindingDesc;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions    = attrDescs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE; // billboards are visible from both sides
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cbAtt{};
    cbAtt.colorWriteMask = 0xF;
    cbAtt.blendEnable    = VK_TRUE;
    cbAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cbAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cbAtt.colorBlendOp        = VK_BLEND_OP_ADD;

    // charDepth attachment (slot 1): impostors don't write to it
    VkPipelineColorBlendAttachmentState cbNoWrite{};
    cbNoWrite.colorWriteMask = 0;
    cbNoWrite.blendEnable    = VK_FALSE;

    VkPipelineColorBlendAttachmentState cbAtts[2] = { cbAtt, cbNoWrite };

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = formats.colorCount > 1 ? 2 : 1;
    cb.pAttachments    = cbAtts;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    auto renderingCI = formats.pipelineRenderingCI();

    VkGraphicsPipelineCreateInfo pCI{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pCI.pNext = &renderingCI;
    pCI.stageCount = 2;
    pCI.pStages = stages;
    pCI.pVertexInputState = &vi;
    pCI.pInputAssemblyState = &ia;
    pCI.pViewportState = &vp;
    pCI.pRasterizationState = &rs;
    pCI.pMultisampleState = &ms;
    pCI.pDepthStencilState = &ds;
    pCI.pColorBlendState = &cb;
    pCI.pDynamicState = &dyn;
    pCI.layout = m_pipelineLayout;

    vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pCI, nullptr, &m_pipeline);

    vkDestroyShaderModule(dev, vmod, nullptr);
    vkDestroyShaderModule(dev, fmod, nullptr);
}

void ImpostorSystem::createCapturePipeline() {
    VkDevice dev = m_device->getDevice();

    // Push constants: viewProj + model
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(CapturePC);

    VkPipelineLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_captureLayout);

    // Vertex input from Model (Vertex struct)
    auto binding = Vertex::getBindingDescription();
    auto attrs   = Vertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions    = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_BACK_BIT;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cbAtt{};
    cbAtt.colorWriteMask = 0xF;
    cbAtt.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments    = &cbAtt;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    // Load capture shaders
    auto loadModule = [&](const std::string& path) -> VkShaderModule {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) return VK_NULL_HANDLE;
        size_t sz = static_cast<size_t>(file.tellg());
        std::vector<char> code(sz);
        file.seekg(0);
        file.read(code.data(), static_cast<std::streamsize>(sz));
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = sz;
        ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod;
        vkCreateShaderModule(dev, &ci, nullptr, &mod);
        return mod;
    };

    VkShaderModule vmod = loadModule("shaders/impostor_capture.vert.spv");
    VkShaderModule fmod = loadModule("shaders/impostor_capture.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName = "main";

    VkFormat colorFmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFmt = m_device->findDepthFormat();
    VkPipelineRenderingCreateInfo rCI{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rCI.colorAttachmentCount = 1;
    rCI.pColorAttachmentFormats = &colorFmt;
    rCI.depthAttachmentFormat = depthFmt;

    VkGraphicsPipelineCreateInfo pCI{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pCI.pNext = &rCI;
    pCI.stageCount = 2;
    pCI.pStages = stages;
    pCI.pVertexInputState = &vi;
    pCI.pInputAssemblyState = &ia;
    pCI.pViewportState = &vp;
    pCI.pRasterizationState = &rs;
    pCI.pMultisampleState = &ms;
    pCI.pDepthStencilState = &ds;
    pCI.pColorBlendState = &cb;
    pCI.pDynamicState = &dyn;
    pCI.layout = m_captureLayout;

    vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pCI, nullptr, &m_capturePipeline);

    vkDestroyShaderModule(dev, vmod, nullptr);
    vkDestroyShaderModule(dev, fmod, nullptr);
}

void ImpostorSystem::ensureInstanceCapacity(size_t needed) {
    if (needed <= m_instanceCapacity) return;

    size_t newCap = std::max(needed, INITIAL_INSTANCE_CAP);
    if (m_instanceCapacity > 0) newCap = std::max(newCap, m_instanceCapacity * 2);

    m_instanceBuffer.destroy();
    m_instanceBuffer = Buffer(m_device->getAllocator(),
                              newCap * sizeof(ImpostorInstance),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_instanceCapacity = newCap;
}

float ImpostorSystem::angleFromDirection(glm::vec2 dir) const {
    return std::atan2(dir.x, dir.y);
}

uint32_t ImpostorSystem::bestAngleIndex(float angleRad, uint32_t angleCount) const {
    float angle = angleRad;
    if (angle < 0) angle += glm::two_pi<float>();
    float step = glm::two_pi<float>() / static_cast<float>(angleCount);
    return static_cast<uint32_t>(std::round(angle / step)) % angleCount;
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
