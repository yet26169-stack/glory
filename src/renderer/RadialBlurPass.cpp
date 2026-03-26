#include "renderer/RadialBlurPass.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>
#include <fstream>
#include <stdexcept>
#include <array>

namespace glory {

// ── File helpers ─────────────────────────────────────────────────────────────
static std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("Failed to open shader: " + path);
    auto sz = static_cast<size_t>(f.tellg());
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
}

// ── init ─────────────────────────────────────────────────────────────────────
void RadialBlurPass::init(const Device& device, const RenderFormats& formats,
                          VkImageView sceneView, VkSampler sampler) {
    m_device  = &device;
    m_formats = formats;
    m_sampler = sampler;

    createDescriptorSetLayout();
    createDescriptorPool();
    createDescriptorSet(sceneView);
    createPipeline();
    spdlog::info("[RadialBlur] Initialized");
}

// ── render ───────────────────────────────────────────────────────────────────
void RadialBlurPass::render(VkCommandBuffer cmd, glm::vec2 center, float intensity,
                            float sampleCount, float falloffStart, float maxBlurDist) {
    if (intensity <= 0.001f) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipeLayout, 0, 1, &m_descSet, 0, nullptr);

    PushConstants pc{};
    pc.center       = center;
    pc.intensity    = intensity;
    pc.sampleCount  = sampleCount;
    pc.falloffStart = falloffStart;
    pc.maxBlurDist  = maxBlurDist;
    pc._pad0        = 0.0f;
    pc._pad1        = 0.0f;

    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstants), &pc);

    vkCmdDraw(cmd, 3, 1, 0, 0); // fullscreen triangle
}

// ── updateDescriptorSet ──────────────────────────────────────────────────────
void RadialBlurPass::updateDescriptorSet(VkImageView sceneView) {
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler     = m_sampler;
    imageInfo.imageView   = sceneView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = m_descSet;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imageInfo;

    vkUpdateDescriptorSets(m_device->getDevice(), 1, &write, 0, nullptr);
}

// ── destroy ──────────────────────────────────────────────────────────────────
void RadialBlurPass::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_pipeline)   { vkDestroyPipeline(dev, m_pipeline, nullptr);             m_pipeline   = VK_NULL_HANDLE; }
    if (m_pipeLayout) { vkDestroyPipelineLayout(dev, m_pipeLayout, nullptr);     m_pipeLayout = VK_NULL_HANDLE; }
    if (m_descPool)   { vkDestroyDescriptorPool(dev, m_descPool, nullptr);       m_descPool   = VK_NULL_HANDLE; }
    if (m_descLayout) { vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr); m_descLayout = VK_NULL_HANDLE; }

    m_descSet = VK_NULL_HANDLE;
    m_device = nullptr;
}

// ── Private: createDescriptorSetLayout ───────────────────────────────────────
void RadialBlurPass::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings    = &binding;

    VK_CHECK(vkCreateDescriptorSetLayout(m_device->getDevice(), &ci, nullptr, &m_descLayout),
             "Create RadialBlur descriptor layout");
}

// ── Private: createDescriptorPool ────────────────────────────────────────────
void RadialBlurPass::createDescriptorPool() {
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};

    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.poolSizeCount = 1;
    ci.pPoolSizes    = &poolSize;
    ci.maxSets       = 1;

    VK_CHECK(vkCreateDescriptorPool(m_device->getDevice(), &ci, nullptr, &m_descPool),
             "Create RadialBlur descriptor pool");
}

// ── Private: createDescriptorSet ─────────────────────────────────────────────
void RadialBlurPass::createDescriptorSet(VkImageView sceneView) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = m_descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &m_descLayout;

    VK_CHECK(vkAllocateDescriptorSets(m_device->getDevice(), &ai, &m_descSet),
             "Allocate RadialBlur descriptor set");

    updateDescriptorSet(sceneView);
}

// ── Private: createPipeline ──────────────────────────────────────────────────
void RadialBlurPass::createPipeline() {
    VkDevice dev = m_device->getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;

    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipeLayout),
             "Create RadialBlur pipeline layout");

    auto vertCode = readFile(std::string(SHADER_DIR) + "tonemap.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "radial_blur.frag.spv");

    VkShaderModule vertMod = createShaderModule(vertCode);
    VkShaderModule fragMod = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth   = 1.0f;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAtt.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments    = &blendAtt;

    std::array<VkDynamicState, 2> dynStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dyn.pDynamicStates    = dynStates.data();

    VkPipelineRenderingCreateInfo fmtCI = m_formats.pipelineRenderingCI();

    VkGraphicsPipelineCreateInfo pipeCI{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeCI.pNext               = &fmtCI;
    pipeCI.renderPass          = VK_NULL_HANDLE;
    pipeCI.stageCount          = 2;
    pipeCI.pStages             = stages;
    pipeCI.pVertexInputState   = &vi;
    pipeCI.pInputAssemblyState = &ia;
    pipeCI.pViewportState      = &vps;
    pipeCI.pRasterizationState = &rs;
    pipeCI.pMultisampleState   = &ms;
    pipeCI.pColorBlendState    = &cb;
    pipeCI.pDynamicState       = &dyn;
    pipeCI.layout              = m_pipeLayout;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_pipeline),
             "Create RadialBlur pipeline");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
}

VkShaderModule RadialBlurPass::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(m_device->getDevice(), &ci, nullptr, &mod),
             "Create RadialBlur shader module");
    return mod;
}

} // namespace glory
