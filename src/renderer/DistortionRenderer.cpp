#include "renderer/DistortionRenderer.h"
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

DistortionRenderer::DistortionRenderer(const Device& device, const RenderFormats& formats, VkImageView sceneColorCopy, VkSampler sampler)
    : m_device(device), m_formats(formats), m_sceneColorCopy(sceneColorCopy), m_sampler(sampler) 
{
    m_sphereMesh = std::make_unique<Model>(Model::createSphere(device, device.getAllocator(), 16, 32));
    createDescriptorSet();
    createPipeline();
}

DistortionRenderer::~DistortionRenderer() {
    VkDevice dev = m_device.getDevice();
    vkDeviceWaitIdle(dev);
    if (m_pipeline) vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    if (m_descPool) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
}

void DistortionRenderer::registerDef(const DistortionDef& def) {
    m_defs[def.id] = def;
}

uint32_t DistortionRenderer::spawn(const std::string& defId, glm::vec3 position) {
    auto it = m_defs.find(defId);
    if (it == m_defs.end()) {
        spdlog::warn("DistortionRenderer: unknown def '{}'", defId);
        return 0;
    }
    DistortionInstance inst;
    inst.handle = m_nextHandle++;
    inst.def = &it->second;
    inst.position = position;
    m_active.push_back(inst);
    return inst.handle;
}

void DistortionRenderer::update(float dt) {
    for (auto it = m_active.begin(); it != m_active.end(); ) {
        it->elapsed += dt;
        if (it->elapsed >= it->def->duration) {
            it = m_active.erase(it);
        } else {
            ++it;
        }
    }
}

void DistortionRenderer::render(VkCommandBuffer cmd, const glm::mat4& viewProj, float appTime, uint32_t width, uint32_t height) {
    if (m_active.empty()) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);

    for (const auto& inst : m_active) {
        DistortionPC pc;
        pc.viewProj = viewProj;
        pc.center = inst.position;
        pc.radius = inst.def->radius;
        pc.strength = inst.def->strength;
        pc.elapsed = inst.elapsed;
        pc.screenSize = glm::vec2(static_cast<float>(width), static_cast<float>(height));

        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DistortionPC), &pc);
        m_sphereMesh->draw(cmd);
    }
}

void DistortionRenderer::updateDescriptorSet(VkImageView sceneColorCopy) {
    m_sceneColorCopy = sceneColorCopy;
    
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView = m_sceneColorCopy;
    imgInfo.sampler = m_sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
}

void DistortionRenderer::createDescriptorSet() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutCI.bindingCount = 1;
    layoutCI.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device.getDevice(), &layoutCI, nullptr, &m_descLayout), "Create Distort Layout");

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolCI.maxSets = 1;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(m_device.getDevice(), &poolCI, nullptr, &m_descPool), "Create Distort Pool");

    VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_device.getDevice(), &allocInfo, &m_descSet), "Alloc Distort Set");

    updateDescriptorSet(m_sceneColorCopy);
}

void DistortionRenderer::createPipeline() {
    VkDevice dev = m_device.getDevice();

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.size = sizeof(DistortionPC);

    VkPipelineLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutCI.setLayoutCount = 1;
    layoutCI.pSetLayouts = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout), "Create Distort Pipe Layout");

    // We can reuse mesh_effect.vert for the vertex shader if we want, or create a specific one.
    // Spec says shaders/distortion.frag. Let's use mesh_effect.vert for vertices.
    auto vertCode = readFile(std::string(SHADER_DIR) + "mesh_effect.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "distortion.frag.spv");

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
    rsCI.polygonMode = VK_POLYGON_MODE_FILL; rsCI.lineWidth = 1.0f; rsCI.cullMode = VK_CULL_MODE_BACK_BIT;

    VkPipelineMultisampleStateCreateInfo msCI{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsCI{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dsCI.depthTestEnable = VK_TRUE; dsCI.depthWriteEnable = VK_FALSE; dsCI.depthCompareOp = VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.colorWriteMask = 0xF;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; // We want to OVERWRITE with distorted sample
    blend.colorBlendOp = VK_BLEND_OP_ADD;

    // Second attachment (charDepth) declared with write-mask=0; distortion only writes to color.
    VkPipelineColorBlendAttachmentState distBlends[2] = {blend, {}};
    VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbCI.attachmentCount = 2;
    cbCI.pAttachments    = distBlends;

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
    VkPipelineRenderingCreateInfo fmtCI = m_formats.pipelineRenderingCI();
    pipeCI.pNext     = &fmtCI;
    pipeCI.renderPass = VK_NULL_HANDLE;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_pipeline), "Create Distort Pipe");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
}

VkShaderModule DistortionRenderer::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(m_device.getDevice(), &ci, nullptr, &module), "Failed Distort shader");
    return module;
}

} // namespace glory
