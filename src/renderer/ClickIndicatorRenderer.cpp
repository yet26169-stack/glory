#include "renderer/ClickIndicatorRenderer.h"
#include "renderer/Device.h"
#include <spdlog/spdlog.h>
#include <fstream>

namespace glory {

struct IndicatorVertex {
    glm::vec3 pos;
    glm::vec2 uv;
};

ClickIndicatorRenderer::ClickIndicatorRenderer(const Device& device, VkRenderPass renderPass)
    : m_device(device) {
    m_texture = std::make_unique<Texture>(device, std::string(ASSET_DIR) + "textures/click_indicator_atlas.png");
    
    createDescriptorSet();
    createPipeline(renderPass);
    createVertexBuffer();
    
    spdlog::info("ClickIndicatorRenderer initialized");
}

ClickIndicatorRenderer::~ClickIndicatorRenderer() {
    VkDevice dev = m_device.getDevice();
    if (m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
}

void ClickIndicatorRenderer::createDescriptorSet() {
    VkDevice dev = m_device.getDevice();
    
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 1;
    layoutCI.pBindings = &binding;
    vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout);
    
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool);
    
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descLayout;
    vkAllocateDescriptorSets(dev, &allocInfo, &m_descSet);
    
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = m_texture->getImageView();
    imageInfo.sampler = m_texture->getSampler();
    
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descSet;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
}

void ClickIndicatorRenderer::createPipeline(VkRenderPass renderPass) {
    VkDevice dev = m_device.getDevice();
    
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(PushConstants);
    
    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pcRange;
    vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout);
    
    auto readFile = [](const std::string& path) {
        std::ifstream f(path, std::ios::ate | std::ios::binary);
        size_t sz = (size_t)f.tellg();
        std::vector<char> buf(sz);
        f.seekg(0); f.read(buf.data(), sz);
        return buf;
    };
    
    auto vcode = readFile(std::string(SHADER_DIR) + "click_indicator.vert.spv");
    auto fcode = readFile(std::string(SHADER_DIR) + "click_indicator.frag.spv");
    
    VkShaderModuleCreateInfo smCI{};
    smCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = vcode.size();
    smCI.pCode = (const uint32_t*)vcode.data();
    VkShaderModule vmod;
    vkCreateShaderModule(dev, &smCI, nullptr, &vmod);
    
    smCI.codeSize = fcode.size();
    smCI.pCode = (const uint32_t*)fcode.data();
    VkShaderModule fmod;
    vkCreateShaderModule(dev, &smCI, nullptr, &fmod);
    
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod;
    stages[1].pName = "main";
    
    VkVertexInputBindingDescription binding{0, sizeof(IndicatorVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(IndicatorVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(IndicatorVertex, uv)};
    
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;
    
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.cullMode = VK_CULL_MODE_NONE;
    
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE; ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    
    VkPipelineColorBlendAttachmentState cbA{};
    cbA.blendEnable = VK_TRUE;
    cbA.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cbA.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cbA.colorBlendOp = VK_BLEND_OP_ADD;
    cbA.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbA.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cbA.alphaBlendOp = VK_BLEND_OP_ADD;
    cbA.colorWriteMask = 0xF;
    VkPipelineColorBlendAttachmentState clickBlends[2] = {cbA, {}};
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 2; cb.pAttachments = clickBlends;
    
    VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyns;
    
    VkGraphicsPipelineCreateInfo gpCI{};
    gpCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpCI.stageCount = 2; gpCI.pStages = stages; gpCI.pVertexInputState = &vi;
    gpCI.pInputAssemblyState = &ia; gpCI.pViewportState = &vp; gpCI.pRasterizationState = &rs;
    gpCI.pMultisampleState = &ms; gpCI.pDepthStencilState = &ds; gpCI.pColorBlendState = &cb;
    gpCI.pDynamicState = &dy; gpCI.layout = m_pipelineLayout; gpCI.renderPass = renderPass;
    
    vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &m_pipeline);
    
    vkDestroyShaderModule(dev, fmod, nullptr);
    vkDestroyShaderModule(dev, vmod, nullptr);
}

void ClickIndicatorRenderer::createVertexBuffer() {
    IndicatorVertex vertices[6] = {
        {{-1.0f, 0.15f, -1.0f}, {0.0f, 0.0f}},
        {{ 1.0f, 0.15f, -1.0f}, {1.0f, 0.0f}},
        {{ 1.0f, 0.15f,  1.0f}, {1.0f, 1.0f}},
        {{-1.0f, 0.15f, -1.0f}, {0.0f, 0.0f}},
        {{ 1.0f, 0.15f,  1.0f}, {1.0f, 1.0f}},
        {{-1.0f, 0.15f,  1.0f}, {0.0f, 1.0f}}
    };
    VkDeviceSize size = sizeof(vertices);
    
    m_vertexBuffer = Buffer(m_device.getAllocator(), size, 
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 
                           VMA_MEMORY_USAGE_CPU_TO_GPU);
    
    std::memcpy(m_vertexBuffer.map(), vertices, (size_t)size);
}

void ClickIndicatorRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj, 
                                    const glm::vec3& center, float t, float size,
                                    const glm::vec4& tint) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    
    PushConstants pc{};
    pc.viewProj = viewProj;
    pc.center = center;
    pc.size = size;
    pc.gridCount = 8;
    pc.frameIndex = 1 + static_cast<int>(t * 54.0f); // Start from frame 1, go up to 55
    pc.tint = tint;
    
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);
    
    VkDeviceSize offsets[] = {0};
    VkBuffer buf = m_vertexBuffer.getBuffer();
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, offsets);
    
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

} // namespace glory
