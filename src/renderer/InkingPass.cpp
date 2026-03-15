#include "renderer/InkingPass.h"
#include "renderer/VkCheck.h"
#include <fstream>
#include <stdexcept>
#include <vector>

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

void InkingPass::init(const Device& device, const RenderFormats& formats,
                      VkImageView characterDepthView, VkSampler sampler) {
    m_device  = &device;
    m_sampler = sampler;
    createDescriptorSet(characterDepthView);
    createPipeline(formats);
}

void InkingPass::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();
    vkDeviceWaitIdle(dev);
    if (m_pipeline)       { vkDestroyPipeline(dev, m_pipeline, nullptr);            m_pipeline       = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_descLayout)     { vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr); m_descLayout    = VK_NULL_HANDLE; }
    if (m_descPool)       { vkDestroyDescriptorPool(dev, m_descPool, nullptr);        m_descPool      = VK_NULL_HANDLE; }
    m_descSet = VK_NULL_HANDLE;
    m_device  = nullptr;
}

void InkingPass::createDescriptorSet(VkImageView characterDepthView) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutCI.bindingCount = 1;
    layoutCI.pBindings    = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device->getDevice(), &layoutCI, nullptr, &m_descLayout), "Create Inking Desc Layout");

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(m_device->getDevice(), &poolCI, nullptr, &m_descPool), "Create Inking Desc Pool");

    VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_device->getDevice(), &allocInfo, &m_descSet), "Alloc Inking Desc Set");

    updateInput(characterDepthView);
}

void InkingPass::updateInput(VkImageView characterDepthView) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;  // matches GENERAL in loadRenderPass subpass ref
    imgInfo.imageView   = characterDepthView;
    imgInfo.sampler     = m_sampler;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_descSet;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;

    vkUpdateDescriptorSets(m_device->getDevice(), 1, &write, 0, nullptr);
}

void InkingPass::createPipeline(const RenderFormats& formats) {
    VkDevice dev = m_device->getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(InkPC);

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout), "Create Inking Pipe Layout");

    auto vertCode = readFile(std::string(SHADER_DIR) + "inking.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "inking.frag.spv");

    VkShaderModule vertMod = createShaderModule(vertCode);
    VkShaderModule fragMod = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo viCI{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    viCI.vertexBindingDescriptionCount   = 0;
    viCI.pVertexBindingDescriptions      = nullptr;
    viCI.vertexAttributeDescriptionCount = 0;
    viCI.pVertexAttributeDescriptions    = nullptr;

    VkPipelineInputAssemblyStateCreateInfo iaCI{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iaCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpCI{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpCI.viewportCount = 1;
    vpCI.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rsCI{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsCI.polygonMode = VK_POLYGON_MODE_FILL;
    rsCI.cullMode    = VK_CULL_MODE_NONE;
    rsCI.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsCI.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo msCI{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsCI{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dsCI.depthTestEnable  = VK_FALSE;
    dsCI.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable         = VK_TRUE;
    blend.colorWriteMask      = 0xF;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;

    // Second attachment (charDepth) must be declared with write-mask=0 so the inking
    // pass leaves it unchanged while still matching the render pass's 2-attachment structure.
    VkPipelineColorBlendAttachmentState inkBlends[2] = {blend, {}};
    VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbCI.attachmentCount = 2;
    cbCI.pAttachments    = inkBlends;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynCI{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynCI.dynamicStateCount = 2;
    dynCI.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pipeCI{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipeCI.stageCount          = 2;
    pipeCI.pStages             = stages;
    pipeCI.pVertexInputState   = &viCI;
    pipeCI.pInputAssemblyState = &iaCI;
    pipeCI.pViewportState      = &vpCI;
    pipeCI.pRasterizationState = &rsCI;
    pipeCI.pMultisampleState   = &msCI;
    pipeCI.pDepthStencilState  = &dsCI;
    pipeCI.pColorBlendState    = &cbCI;
    pipeCI.pDynamicState       = &dynCI;
    VkPipelineRenderingCreateInfo fmtCI = formats.pipelineRenderingCI();
    pipeCI.pNext               = &fmtCI;
    pipeCI.layout              = m_pipelineLayout;
    pipeCI.renderPass          = VK_NULL_HANDLE;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_pipeline), "Create Inking Pipeline");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
}

VkShaderModule InkingPass::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(m_device->getDevice(), &ci, nullptr, &module), "Create Inking Shader Module");
    return module;
}

void InkingPass::render(VkCommandBuffer cmd, float threshold, float thickness,
                        const glm::vec4& inkColor) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 1, &m_descSet, 0, nullptr);
    InkPC pc{ inkColor, threshold, thickness };
    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(InkPC), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace glory
