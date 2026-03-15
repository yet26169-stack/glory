#include "renderer/OutlineRenderer.h"
#include "renderer/Buffer.h"
#include "renderer/StaticSkinnedMesh.h"
#include "renderer/VkCheck.h"

#include <fstream>
#include <stdexcept>
#include <vector>
#include <array>

namespace glory {

// ── Helper: load compiled SPIR-V ─────────────────────────────────────────────
static std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("OutlineRenderer: cannot open " + path);
    auto sz = static_cast<size_t>(file.tellg());
    std::vector<char> buf(sz);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
}

static VkShaderModule makeModule(VkDevice dev, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod), "Create outline shader module");
    return mod;
}

// ── Outline vertex input: same layout as the skinned pipeline ────────────────
static void buildSkinnedVertexInput(
    VkPipelineVertexInputStateCreateInfo&        viCI,
    std::array<VkVertexInputBindingDescription, 2>& bindings,
    std::vector<VkVertexInputAttributeDescription>&  attrs)
{
    auto skinnedBind  = SkinnedVertex::getBindingDescription();
    auto skinnedAttrs = SkinnedVertex::getAttributeDescriptions();
    auto instBind     = InstanceData::getBindingDescription();
    auto instAttrs    = InstanceData::getAttributeDescriptions();
    instBind.binding  = 1;
    for (auto& a : instAttrs) { a.binding = 1; a.location += 2; }

    bindings[0] = skinnedBind;
    bindings[1] = instBind;

    attrs.insert(attrs.end(), skinnedAttrs.begin(), skinnedAttrs.end());
    attrs.insert(attrs.end(), instAttrs.begin(), instAttrs.end());

    viCI.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viCI.vertexBindingDescriptionCount   = 2;
    viCI.pVertexBindingDescriptions      = bindings.data();
    viCI.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    viCI.pVertexAttributeDescriptions    = attrs.data();
}

// ── Init / Destroy ────────────────────────────────────────────────────────────
void OutlineRenderer::init(const Device& device,
                           VkRenderPass  renderPass,
                           VkDescriptorSetLayout mainLayout) {
    m_device = &device;
    createPipelineLayout(mainLayout);
    createPipelines(renderPass);
}

void OutlineRenderer::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();
    vkDeviceWaitIdle(dev);
    if (m_outlineDrawPipeline)  { vkDestroyPipeline(dev, m_outlineDrawPipeline,  nullptr); m_outlineDrawPipeline  = VK_NULL_HANDLE; }
    if (m_stencilWritePipeline) { vkDestroyPipeline(dev, m_stencilWritePipeline, nullptr); m_stencilWritePipeline = VK_NULL_HANDLE; }
    if (m_outlineLayout)        { vkDestroyPipelineLayout(dev, m_outlineLayout,  nullptr); m_outlineLayout        = VK_NULL_HANDLE; }
    m_device = nullptr;
}

// ── Pipeline layout ───────────────────────────────────────────────────────────
void OutlineRenderer::createPipelineLayout(VkDescriptorSetLayout mainLayout) {
    // One push constant range covering both vertex and fragment stages.
    // Vertex reads  pc.boneBaseIndex (offset 0) and pc.outlineScale (offset 4).
    // Fragment reads pc.outlineColor (offset 16) via the inOutlineColor varying.
    // The stencil-write pipeline only pushes boneBaseIndex at offset 0 into its
    // own PC block (skinned.vert declares boneBaseIndex at offset 0 in its PC),
    // but the layout range is large enough to satisfy Vulkan validation.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(OutlinePC); // 32 bytes

    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 1;
    lci.pSetLayouts            = &mainLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(m_device->getDevice(), &lci, nullptr, &m_outlineLayout),
             "Create outline pipeline layout");
}

// ── Pipeline creation ─────────────────────────────────────────────────────────
void OutlineRenderer::createPipelines(VkRenderPass renderPass) {
    VkDevice dev = m_device->getDevice();

    // ── Shared: vertex input (skinned mesh + per-instance data) ──────────────
    std::array<VkVertexInputBindingDescription, 2> bindings{};
    std::vector<VkVertexInputAttributeDescription>  attrs;
    VkPipelineVertexInputStateCreateInfo viCI{};
    buildSkinnedVertexInput(viCI, bindings, attrs);

    VkPipelineInputAssemblyStateCreateInfo iaCI{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iaCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpCI{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpCI.viewportCount = 1;
    vpCI.scissorCount  = 1;

    VkPipelineMultisampleStateCreateInfo msCI{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynCI{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynCI.dynamicStateCount = 2;
    dynCI.pDynamicStates    = dynStates;

    // ── Both color attachments masked off (no color writes in either pass) ────
    // Stencil write:  writes stencil only
    // Outline draw:   will be overridden below for pass 2
    VkPipelineColorBlendAttachmentState blendOff{};
    blendOff.colorWriteMask = 0; // discard

    // ── Pass 1: stencil write pipeline ───────────────────────────────────────
    {
        auto vertCode = readFile(std::string(SHADER_DIR) + "skinned.vert.spv");
        auto fragCode = readFile(std::string(SHADER_DIR) + "outline_stencil.frag.spv");
        VkShaderModule vertMod = makeModule(dev, vertCode);
        VkShaderModule fragMod = makeModule(dev, fragCode);

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragMod;
        stages[1].pName  = "main";

        VkPipelineRasterizationStateCreateInfo rsCI{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rsCI.polygonMode = VK_POLYGON_MODE_FILL;
        rsCI.cullMode    = VK_CULL_MODE_BACK_BIT;
        rsCI.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rsCI.lineWidth   = 1.0f;

        // Stencil: always pass, write ref=1
        VkStencilOpState stencilOp{};
        stencilOp.failOp      = VK_STENCIL_OP_KEEP;
        stencilOp.passOp      = VK_STENCIL_OP_REPLACE;
        stencilOp.depthFailOp = VK_STENCIL_OP_KEEP;
        stencilOp.compareOp   = VK_COMPARE_OP_ALWAYS;
        stencilOp.compareMask = 0xFF;
        stencilOp.writeMask   = 0xFF;
        stencilOp.reference   = 1;

        VkPipelineDepthStencilStateCreateInfo dsCI{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dsCI.depthTestEnable       = VK_TRUE;
        dsCI.depthWriteEnable      = VK_TRUE;
        dsCI.depthCompareOp        = VK_COMPARE_OP_LESS;
        dsCI.stencilTestEnable     = VK_TRUE;
        dsCI.front                 = stencilOp;
        dsCI.back                  = stencilOp;

        // Disable color writes to BOTH attachments (stencil-only pass)
        std::array<VkPipelineColorBlendAttachmentState, 2> blends{ blendOff, blendOff };
        VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbCI.attachmentCount = 2;
        cbCI.pAttachments    = blends.data();

        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &viCI;
        pci.pInputAssemblyState = &iaCI;
        pci.pViewportState      = &vpCI;
        pci.pRasterizationState = &rsCI;
        pci.pMultisampleState   = &msCI;
        pci.pDepthStencilState  = &dsCI;
        pci.pColorBlendState    = &cbCI;
        pci.pDynamicState       = &dynCI;
        pci.layout              = m_outlineLayout;
        pci.renderPass          = renderPass;
        pci.subpass             = 0;

        VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_stencilWritePipeline),
                 "Create stencil-write pipeline");

        vkDestroyShaderModule(dev, vertMod, nullptr);
        vkDestroyShaderModule(dev, fragMod, nullptr);
    }

    // ── Pass 2: outline draw pipeline ─────────────────────────────────────────
    {
        auto vertCode = readFile(std::string(SHADER_DIR) + "outline.vert.spv");
        auto fragCode = readFile(std::string(SHADER_DIR) + "outline.frag.spv");
        VkShaderModule vertMod = makeModule(dev, vertCode);
        VkShaderModule fragMod = makeModule(dev, fragCode);

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragMod;
        stages[1].pName  = "main";

        // Front-face culling: renders the inflated back-face shell visible only
        // where the original silhouette doesn't cover (stencil NOT_EQUAL).
        VkPipelineRasterizationStateCreateInfo rsCI{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rsCI.polygonMode = VK_POLYGON_MODE_FILL;
        rsCI.cullMode    = VK_CULL_MODE_FRONT_BIT;
        rsCI.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rsCI.lineWidth   = 1.0f;

        // Stencil: draw only where NOT covered by the silhouette (ref=1 was set above)
        VkStencilOpState stencilOp{};
        stencilOp.failOp      = VK_STENCIL_OP_KEEP;
        stencilOp.passOp      = VK_STENCIL_OP_KEEP;
        stencilOp.depthFailOp = VK_STENCIL_OP_KEEP;
        stencilOp.compareOp   = VK_COMPARE_OP_NOT_EQUAL;
        stencilOp.compareMask = 0xFF;
        stencilOp.writeMask   = 0x00;
        stencilOp.reference   = 1;

        VkPipelineDepthStencilStateCreateInfo dsCI{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dsCI.depthTestEnable       = VK_TRUE;
        dsCI.depthWriteEnable      = VK_FALSE; // outline doesn't occlude scene
        dsCI.depthCompareOp        = VK_COMPARE_OP_LESS_OR_EQUAL;
        dsCI.stencilTestEnable     = VK_TRUE;
        dsCI.front                 = stencilOp;
        dsCI.back                  = stencilOp;

        // Attachment 0: full RGBA write for the outline color
        VkPipelineColorBlendAttachmentState blendOn{};
        blendOn.blendEnable    = VK_FALSE;
        blendOn.colorWriteMask = 0xF;

        // Attachment 1 (charDepth): no write — outline shell shouldn't contaminate it
        std::array<VkPipelineColorBlendAttachmentState, 2> blends{ blendOn, blendOff };
        VkPipelineColorBlendStateCreateInfo cbCI{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbCI.attachmentCount = 2;
        cbCI.pAttachments    = blends.data();

        VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &viCI;
        pci.pInputAssemblyState = &iaCI;
        pci.pViewportState      = &vpCI;
        pci.pRasterizationState = &rsCI;
        pci.pMultisampleState   = &msCI;
        pci.pDepthStencilState  = &dsCI;
        pci.pColorBlendState    = &cbCI;
        pci.pDynamicState       = &dynCI;
        pci.layout              = m_outlineLayout;
        pci.renderPass          = renderPass;
        pci.subpass             = 0;

        VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_outlineDrawPipeline),
                 "Create outline draw pipeline");

        vkDestroyShaderModule(dev, vertMod, nullptr);
        vkDestroyShaderModule(dev, fragMod, nullptr);
    }
}

// ── Render ────────────────────────────────────────────────────────────────────
void OutlineRenderer::renderOutline(VkCommandBuffer cmd,
                                    VkDescriptorSet ds,
                                    VkBuffer        instBuf,
                                    VkDeviceSize    instOffset,
                                    uint32_t        boneBase,
                                    const StaticSkinnedMesh& mesh,
                                    float           outlineScale,
                                    const glm::vec4& outlineColor) {
    // ── Pass 1: write stencil=1 into the character silhouette ────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_stencilWritePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_outlineLayout, 0, 1, &ds, 0, nullptr);
    vkCmdBindVertexBuffers(cmd, 1, 1, &instBuf, &instOffset);
    // skinned.vert reads boneBaseIndex at offset 0 in its own PC block
    vkCmdPushConstants(cmd, m_outlineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t), &boneBase);
    mesh.bind(cmd);
    mesh.draw(cmd);

    // ── Pass 2: draw inflated shell where stencil != 1 ───────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_outlineDrawPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_outlineLayout, 0, 1, &ds, 0, nullptr);
    vkCmdBindVertexBuffers(cmd, 1, 1, &instBuf, &instOffset);

    OutlinePC pc{};
    pc.boneBaseIndex = boneBase;
    pc.outlineScale  = outlineScale;
    pc.outlineColor  = outlineColor;
    vkCmdPushConstants(cmd, m_outlineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(OutlinePC), &pc);
    mesh.bind(cmd);
    mesh.draw(cmd);
}

} // namespace glory
