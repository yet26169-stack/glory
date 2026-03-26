#include "renderer/DeferredDecalRenderer.h"
#include "renderer/VkCheck.h"
#include "renderer/BindlessDescriptors.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace glory {

// ── Unit cube geometry ([-0.5, 0.5]³) ──────────────────────────────────────
static constexpr float cubeVertices[] = {
    // Front face
    -0.5f, -0.5f,  0.5f,
     0.5f, -0.5f,  0.5f,
     0.5f,  0.5f,  0.5f,
    -0.5f,  0.5f,  0.5f,
    // Back face
    -0.5f, -0.5f, -0.5f,
     0.5f, -0.5f, -0.5f,
     0.5f,  0.5f, -0.5f,
    -0.5f,  0.5f, -0.5f,
};

static constexpr uint16_t cubeIndices[] = {
    // Front
    0, 1, 2, 2, 3, 0,
    // Back
    5, 4, 7, 7, 6, 5,
    // Left
    4, 0, 3, 3, 7, 4,
    // Right
    1, 5, 6, 6, 2, 1,
    // Top
    3, 2, 6, 6, 7, 3,
    // Bottom
    4, 5, 1, 1, 0, 4,
};

// ═════════════════════════════════════════════════════════════════════════════

DeferredDecalRenderer::DeferredDecalRenderer(const Device& device,
                                             const RenderFormats& formats,
                                             BindlessDescriptors& bindless)
    : m_device(device), m_bindless(bindless) {
    m_decals.reserve(MAX_DECALS);
    createCubeMesh();
    // Descriptor/pipeline creation is deferred until first render()
    // when we have a valid depthView.
}

DeferredDecalRenderer::~DeferredDecalRenderer() {
    destroyAll();
}

void DeferredDecalRenderer::createCubeMesh() {
    m_cubeVBO = Buffer::createDeviceLocal(
        m_device, m_device.getAllocator(),
        cubeVertices, sizeof(cubeVertices),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    m_cubeIBO = Buffer::createDeviceLocal(
        m_device, m_device.getAllocator(),
        cubeIndices, sizeof(cubeIndices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    m_cubeIndexCount = sizeof(cubeIndices) / sizeof(cubeIndices[0]);

    // UBO for per-frame data (viewProj + screenSize + near/far)
    m_ubo = Buffer(m_device.getAllocator(), sizeof(DecalUBO),
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_uboMapped = m_ubo.map();
}

void DeferredDecalRenderer::createDescriptors(VkImageView depthView,
                                               VkSampler depthSampler) {
    VkDevice dev = m_device.getDevice();

    // Descriptor set layout: binding 0 = UBO, binding 1 = depth sampler
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout),
             "deferred decal desc layout");

    // Descriptor pool
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};

    VkDescriptorPoolCreateInfo poolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolCI.pPoolSizes    = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool),
             "deferred decal desc pool");

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descLayout;
    VK_CHECK(vkAllocateDescriptorSets(dev, &allocInfo, &m_descSet),
             "deferred decal desc alloc");

    // Write descriptors
    VkDescriptorBufferInfo uboInfo{};
    uboInfo.buffer = m_ubo.getBuffer();
    uboInfo.offset = 0;
    uboInfo.range  = sizeof(DecalUBO);

    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler     = depthSampler;
    depthInfo.imageView   = depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet          = m_descSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo     = &uboInfo;

    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet          = m_descSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo      = &depthInfo;

    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void DeferredDecalRenderer::createPipeline(const RenderFormats& formats) {
    VkDevice dev = m_device.getDevice();

    auto readFile = [](const std::string& path) -> std::vector<char> {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("Shader not found: " + path);
        size_t sz = static_cast<size_t>(file.tellg());
        std::vector<char> buf(sz);
        file.seekg(0); file.read(buf.data(), static_cast<std::streamsize>(sz));
        return buf;
    };
    auto makeModule = [&](const std::vector<char>& code) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size();
        ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m), "deferred decal shader module");
        return m;
    };

    VkShaderModule vert = makeModule(readFile(std::string(SHADER_DIR) + "deferred_decal.vert.spv"));
    VkShaderModule frag = makeModule(readFile(std::string(SHADER_DIR) + "deferred_decal.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vert, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main"};

    // Vertex input: position only (vec3)
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(float) * 3;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attr{};
    attr.binding  = 0;
    attr.location = 0;
    attr.format   = VK_FORMAT_R32G32B32_SFLOAT;
    attr.offset   = 0;

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions    = &attr;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynStates;

    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount  = 1;

    // Cull front faces: we render the back faces of the cube so pixels inside
    // the box always get rasterized (even when the camera is inside the box).
    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.lineWidth   = 1.0f;
    rast.cullMode    = VK_CULL_MODE_FRONT_BIT;
    rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test ON (reversed-Z: GREATER), depth write OFF (read-only)
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable  = VK_FALSE;  // We do depth test manually in fragment shader
    ds.depthWriteEnable = VK_FALSE;

    // Alpha blending (premultiplied-alpha compatible)
    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.blendEnable         = VK_TRUE;
    blendAttach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttach.colorBlendOp        = VK_BLEND_OP_ADD;
    blendAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttach.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendAttach.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    // charDepth attachment: no write
    VkPipelineColorBlendAttachmentState charDepthBlend{};
    charDepthBlend.colorWriteMask = 0;  // Don't write to charDepth

    VkPipelineColorBlendAttachmentState blendAttachments[2] = {blendAttach, charDepthBlend};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 2;
    cb.pAttachments    = blendAttachments;

    // Pipeline layout: set 0 = decal UBO + depth, set 1 = bindless
    VkDescriptorSetLayout setLayouts[2] = {m_descLayout, m_bindless.getLayout()};
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.size       = sizeof(DecalPushConstants);

    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount         = 2;
    lci.pSetLayouts            = setLayouts;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pc;
    VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &m_pipelineLayout),
             "deferred decal pipeline layout");

    VkPipelineRenderingCreateInfo dynCI = formats.pipelineRenderingCI();

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext               = &dynCI;
    pci.stageCount          = 2;
    pci.pStages             = stages;
    pci.pVertexInputState   = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState      = &vps;
    pci.pRasterizationState = &rast;
    pci.pMultisampleState   = &ms;
    pci.pDepthStencilState  = &ds;
    pci.pColorBlendState    = &cb;
    pci.pDynamicState       = &dyn;
    pci.layout              = m_pipelineLayout;
    pci.renderPass          = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_pipeline),
             "deferred decal pipeline");

    vkDestroyShaderModule(dev, frag, nullptr);
    vkDestroyShaderModule(dev, vert, nullptr);

    spdlog::info("DeferredDecalRenderer: pipeline created");
}

void DeferredDecalRenderer::addDecal(const DeferredDecal& decal) {
    ActiveDecal ad;
    ad.decal          = decal;
    ad.elapsed        = 0.0f;
    ad.currentOpacity = decal.opacity;

    if (m_decals.size() >= MAX_DECALS) {
        // Ring-buffer: replace oldest
        m_decals.erase(m_decals.begin());
    }
    m_decals.push_back(ad);
}

void DeferredDecalRenderer::update(float dt) {
    for (auto it = m_decals.begin(); it != m_decals.end(); ) {
        it->elapsed += dt;

        if (it->decal.lifetime > 0.0f) {
            float remaining = it->decal.lifetime - it->elapsed;
            if (remaining <= 0.0f) {
                it = m_decals.erase(it);
                continue;
            }
            // Fade out during the last fadeTime seconds
            if (remaining < it->decal.fadeTime) {
                it->currentOpacity = it->decal.opacity * (remaining / it->decal.fadeTime);
            } else {
                it->currentOpacity = it->decal.opacity;
            }
        }
        ++it;
    }
}

void DeferredDecalRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj,
                                    VkImageView depthView, VkSampler depthSampler,
                                    const glm::vec2& screenSize,
                                    float nearPlane, float farPlane) {
    if (m_decals.empty()) return;

    // Lazy initialization on first render (need depthView)
    if (m_descLayout == VK_NULL_HANDLE) {
        RenderFormats formats = RenderFormats::hdrMain(
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_R32_SFLOAT);
        createDescriptors(depthView, depthSampler);
        createPipeline(formats);
    }

    // Update UBO
    DecalUBO uboData{};
    uboData.viewProj   = viewProj;
    uboData.screenSize = screenSize;
    uboData.nearPlane  = nearPlane;
    uboData.farPlane   = farPlane;
    std::memcpy(m_uboMapped, &uboData, sizeof(DecalUBO));

    // Bind pipeline and descriptors
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    VkDescriptorSet sets[2] = {m_descSet, m_bindless.getSet()};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 2, sets, 0, nullptr);

    // Bind cube mesh
    VkBuffer vb = m_cubeVBO.getBuffer();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, m_cubeIBO.getBuffer(), 0, VK_INDEX_TYPE_UINT16);

    // Draw each decal
    for (const auto& ad : m_decals) {
        DecalPushConstants pc{};
        pc.invDecalModel = glm::inverse(ad.decal.transform);
        pc.decalColor    = ad.decal.color;
        pc.opacity       = ad.currentOpacity;
        pc.fadeDistance   = ad.decal.fadeDistance;
        pc.texIdx        = ad.decal.textureIndex;
        pc._pad          = 0.0f;

        vkCmdPushConstants(cmd, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(DecalPushConstants), &pc);
        vkCmdDrawIndexed(cmd, m_cubeIndexCount, 1, 0, 0, 0);
    }
}

void DeferredDecalRenderer::updateDepthDescriptor(VkImageView depthView,
                                                    VkSampler depthSampler) {
    if (m_descSet == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler     = depthSampler;
    depthInfo.imageView   = depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = m_descSet;
    write.dstBinding      = 1;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &depthInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
}

void DeferredDecalRenderer::destroyAll() {
    VkDevice dev = m_device.getDevice();

    m_decals.clear();

    if (m_pipeline)       { vkDestroyPipeline(dev, m_pipeline, nullptr);             m_pipeline       = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_descPool)       { vkDestroyDescriptorPool(dev, m_descPool, nullptr);       m_descPool       = VK_NULL_HANDLE; }
    if (m_descLayout)     { vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr); m_descLayout    = VK_NULL_HANDLE; }

    if (m_uboMapped) { m_ubo.unmap(); m_uboMapped = nullptr; }
    m_ubo.destroy();
    m_cubeVBO.destroy();
    m_cubeIBO.destroy();

    m_descSet = VK_NULL_HANDLE;
}

} // namespace glory
