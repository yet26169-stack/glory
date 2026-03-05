#include "renderer/PostProcess.h"
#include "renderer/Buffer.h"
#include "renderer/Device.h"
#include "renderer/Image.h"
#include "renderer/Swapchain.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <array>
#include <fstream>

namespace glory {

namespace {
std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("Failed to open shader file: " + path);
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(fileSize));
    return buffer;
}

VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &module),
             "Failed to create shader module");
    return module;
}
} // anonymous namespace

PostProcess::PostProcess(const Device& device, const Swapchain& swapchain)
    : m_device(device)
{
    createSampler();

    // 1×1 white dummy images — valid Vulkan objects bound to SSAO/Bloom
    // descriptor slots when those passes are skipped in MOBA mode.
    // Using R8G8B8A8_UNORM for sampled-image compatibility.
    m_dummyBloomImage = Image(device, 1, 1,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    m_dummySSAOImage  = Image(device, 1, 1,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    // Transition dummy images from UNDEFINED → SHADER_READ_ONLY_OPTIMAL
    // so descriptor sets referencing them are valid from the first frame.
    {
      VkCommandBuffer cmd;
      VkCommandBufferAllocateInfo cbAI{};
      cbAI.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      cbAI.commandPool        = device.getGraphicsCommandPool();
      cbAI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cbAI.commandBufferCount = 1;
      vkAllocateCommandBuffers(device.getDevice(), &cbAI, &cmd);

      VkCommandBufferBeginInfo beginInfo{};
      beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkBeginCommandBuffer(cmd, &beginInfo);

      VkImageMemoryBarrier barriers[2]{};
      VkImage dummies[] = { m_dummyBloomImage.getImage(), m_dummySSAOImage.getImage() };
      for (int i = 0; i < 2; ++i) {
        barriers[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].srcAccessMask       = 0;
        barriers[i].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image               = dummies[i];
        barriers[i].subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      }
      vkCmdPipelineBarrier(cmd,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          0, 0, nullptr, 0, nullptr, 2, barriers);

      vkEndCommandBuffer(cmd);
      VkSubmitInfo submitInfo{};
      submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submitInfo.commandBufferCount = 1;
      submitInfo.pCommandBuffers    = &cmd;
      vkQueueSubmit(device.getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
      vkQueueWaitIdle(device.getGraphicsQueue());
      vkFreeCommandBuffers(device.getDevice(), device.getGraphicsCommandPool(), 1, &cmd);
    }

    createHDRResources(swapchain);
    createHDRRenderPass(swapchain);
    createHDRFramebuffer(swapchain);
    createOutputRenderPass(swapchain);
    createOutputFramebuffers(swapchain);
    createDescriptors();
    updateDescriptors();
    createPipeline(swapchain);
    spdlog::info("Post-processing pipeline created");
}

PostProcess::~PostProcess() { cleanup(); }

void PostProcess::cleanup() {
    if (m_cleaned) return;
    m_cleaned = true;

    VkDevice dev = m_device.getDevice();

    destroySwapchainResources();

    if (m_pipeline)       vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_mobaPipeline)   vkDestroyPipeline(dev, m_mobaPipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_renderPass)     vkDestroyRenderPass(dev, m_renderPass, nullptr);
    if (m_hdrRenderPass)  vkDestroyRenderPass(dev, m_hdrRenderPass, nullptr);

    if (m_descPool)   vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descLayout) vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);
    if (m_sampler)    vkDestroySampler(dev, m_sampler, nullptr);

    spdlog::info("Post-processing destroyed");
}

void PostProcess::recreate(const Swapchain& swapchain) {
    destroySwapchainResources();
    createHDRResources(swapchain);
    createHDRFramebuffer(swapchain);
    createOutputFramebuffers(swapchain);
    updateDescriptors();
}

void PostProcess::destroySwapchainResources() {
    VkDevice dev = m_device.getDevice();
    if (m_hdrFramebuffer) vkDestroyFramebuffer(dev, m_hdrFramebuffer, nullptr);
    m_hdrFramebuffer = VK_NULL_HANDLE;
    for (auto fb : m_framebuffers) vkDestroyFramebuffer(dev, fb, nullptr);
    m_framebuffers.clear();
    m_hdrImage      = Image{};
    m_hdrDepthImage = Image{};
}

// ── HDR offscreen target ────────────────────────────────────────────────────
void PostProcess::createHDRResources(const Swapchain& swapchain) {
    auto ext = swapchain.getExtent();
    m_hdrImage = Image(m_device, ext.width, ext.height,
                       VK_FORMAT_R16G16B16A16_SFLOAT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
    m_hdrDepthImage = Image(m_device, ext.width, ext.height,
                            m_device.findDepthFormat(),
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT);
}

void PostProcess::createHDRRenderPass(const Swapchain&) {
    std::array<VkAttachmentDescription, 2> attachments{};

    // Color (HDR)
    attachments[0].format         = VK_FORMAT_R16G16B16A16_SFLOAT;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Depth
    attachments[1].format         = m_device.findDepthFormat();
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = static_cast<uint32_t>(attachments.size());
    ci.pAttachments    = attachments.data();
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    VK_CHECK(vkCreateRenderPass(m_device.getDevice(), &ci, nullptr, &m_hdrRenderPass),
             "Failed to create HDR render pass");
}

void PostProcess::createHDRFramebuffer(const Swapchain& swapchain) {
    auto ext = swapchain.getExtent();
    VkImageView views[] = {m_hdrImage.getImageView(), m_hdrDepthImage.getImageView()};

    VkFramebufferCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass      = m_hdrRenderPass;
    ci.attachmentCount = 2;
    ci.pAttachments    = views;
    ci.width           = ext.width;
    ci.height          = ext.height;
    ci.layers          = 1;

    VK_CHECK(vkCreateFramebuffer(m_device.getDevice(), &ci, nullptr, &m_hdrFramebuffer),
             "Failed to create HDR framebuffer");
}

// ── Output (to swapchain) ───────────────────────────────────────────────────
void PostProcess::createOutputRenderPass(const Swapchain& swapchain) {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = swapchain.getImageFormat();
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments    = &colorAttachment;
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    VK_CHECK(vkCreateRenderPass(m_device.getDevice(), &ci, nullptr, &m_renderPass),
             "Failed to create post-process render pass");
}

void PostProcess::createOutputFramebuffers(const Swapchain& swapchain) {
    auto& imageViews = swapchain.getImageViews();
    auto ext = swapchain.getExtent();

    m_framebuffers.resize(imageViews.size());
    for (size_t i = 0; i < imageViews.size(); ++i) {
        VkFramebufferCreateInfo ci{};
        ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass      = m_renderPass;
        ci.attachmentCount = 1;
        ci.pAttachments    = &imageViews[i];
        ci.width           = ext.width;
        ci.height          = ext.height;
        ci.layers          = 1;

        VK_CHECK(vkCreateFramebuffer(m_device.getDevice(), &ci, nullptr, &m_framebuffers[i]),
                 "Failed to create post-process framebuffer");
    }
}

void PostProcess::createSampler() {
    VkSamplerCreateInfo ci{};
    ci.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_LINEAR;
    ci.minFilter = VK_FILTER_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    VK_CHECK(vkCreateSampler(m_device.getDevice(), &ci, nullptr, &m_sampler),
             "Failed to create post-process sampler");
}

void PostProcess::createDescriptors() {
    VkDevice dev = m_device.getDevice();

    // Four bindings: 0=HDR scene, 1=bloom, 2=SSAO, 3=depth
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutCI.pBindings    = bindings.data();

    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_descLayout),
             "Failed to create post-process desc layout");

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 4;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    poolCI.maxSets       = 1;

    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool),
             "Failed to create post-process desc pool");

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descLayout;

    VK_CHECK(vkAllocateDescriptorSets(dev, &allocInfo, &m_descSet),
             "Failed to allocate post-process descriptor set");
}

void PostProcess::updateDescriptors() {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = m_sampler;
    imgInfo.imageView   = m_hdrImage.getImageView();
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_descSet;
    write.dstBinding      = 0;
    write.dstArrayElement = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
}

void PostProcess::createPipeline(const Swapchain&) {
    VkDevice dev = m_device.getDevice();

    auto vertCode = readFile(std::string(SHADER_DIR) + "postprocess.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "postprocess.frag.spv");
    VkShaderModule vertModule = createShaderModule(dev, vertCode);
    VkShaderModule fragModule = createShaderModule(dev, fragCode);

    // ── Specialization constant layout (13 entries) ────────────────────────
    // constant_id: 0=bloom 1=ssao 2=chroma 3=godRays 4=dof 5=autoExp
    //              6=heat  7=outline 8=sharpen 9=grain 10=dither 11=fxaa
    //              12=toneMapMode
    struct SpecData {
        int32_t enableBloom       = 1;
        int32_t enableSSAO        = 1;
        int32_t enableChroma      = 1;
        int32_t enableGodRays     = 1;
        int32_t enableDOF         = 1;
        int32_t enableAutoExp     = 1;
        int32_t enableHeat        = 1;
        int32_t enableOutline     = 1;
        int32_t enableSharpen     = 1;
        int32_t enableFilmGrain   = 1;
        int32_t enableDither      = 1;
        int32_t enableFXAA        = 1;
        int32_t toneMapMode       = 0; // ACES
    };

    static constexpr uint32_t SPEC_COUNT = 13;
    VkSpecializationMapEntry mapEntries[SPEC_COUNT];
    for (uint32_t i = 0; i < SPEC_COUNT; ++i) {
        mapEntries[i].constantID = i;
        mapEntries[i].offset     = i * sizeof(int32_t);
        mapEntries[i].size       = sizeof(int32_t);
    }

    // Full-quality pipeline: all features enabled
    SpecData fullSpec{};
    VkSpecializationInfo fullSpecInfo{};
    fullSpecInfo.mapEntryCount = SPEC_COUNT;
    fullSpecInfo.pMapEntries   = mapEntries;
    fullSpecInfo.dataSize      = sizeof(SpecData);
    fullSpecInfo.pData         = &fullSpec;

    // MOBA-mode pipeline: only FXAA + tone mapping, everything else compiled out
    SpecData mobaSpec{};
    mobaSpec.enableBloom     = 0;
    mobaSpec.enableSSAO      = 0;
    mobaSpec.enableChroma    = 0;
    mobaSpec.enableGodRays   = 0;
    mobaSpec.enableDOF       = 0;
    mobaSpec.enableAutoExp   = 0;
    mobaSpec.enableHeat      = 0;
    mobaSpec.enableOutline   = 0;
    mobaSpec.enableSharpen   = 0;
    mobaSpec.enableFilmGrain = 0;
    mobaSpec.enableDither    = 0;
    mobaSpec.enableFXAA      = 1;
    mobaSpec.toneMapMode     = 0;
    VkSpecializationInfo mobaSpecInfo{};
    mobaSpecInfo.mapEntryCount = SPEC_COUNT;
    mobaSpecInfo.pMapEntries   = mapEntries;
    mobaSpecInfo.dataSize      = sizeof(SpecData);
    mobaSpecInfo.pData         = &mobaSpec;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName  = "main";
    // Full-quality: attach full spec info to frag stage
    stages[1].pSpecializationInfo = &fullSpecInfo;

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttachment;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dynStates;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(PostProcessParams);

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout),
             "Failed to create post-process pipeline layout");

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.stageCount          = 2;
    pipelineCI.pStages             = stages;
    pipelineCI.pVertexInputState   = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState      = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState   = &multisample;
    pipelineCI.pColorBlendState    = &colorBlend;
    pipelineCI.pDynamicState       = &dynState;
    pipelineCI.layout              = m_pipelineLayout;
    pipelineCI.renderPass          = m_renderPass;
    pipelineCI.subpass             = 0;

    // Build both pipelines in one call (cheaper than two separate calls)
    VkPipeline pipelines[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkGraphicsPipelineCreateInfo pipelineCIs[2] = {pipelineCI, pipelineCI};
    // MOBA pipeline: swap frag specialization info
    pipelineCIs[1].pStages = nullptr; // must copy stages array per pipeline
    VkPipelineShaderStageCreateInfo mobaStages[2] = {stages[0], stages[1]};
    mobaStages[1].pSpecializationInfo = &mobaSpecInfo;
    pipelineCIs[1] = pipelineCI;
    pipelineCIs[1].pStages = mobaStages;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 2, pipelineCIs,
                                       nullptr, pipelines),
             "Failed to create post-process pipelines");
    m_pipeline     = pipelines[0];
    m_mobaPipeline = pipelines[1];

    vkDestroyShaderModule(dev, fragModule, nullptr);
    vkDestroyShaderModule(dev, vertModule, nullptr);
    spdlog::info("Post-process pipelines created (full + MOBA-optimized)");
}

void PostProcess::updateBloomDescriptor(VkImageView bloomView) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = m_sampler;
    imgInfo.imageView   = bloomView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_descSet;
    write.dstBinding      = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
}

void PostProcess::updateSSAODescriptor(VkImageView ssaoView) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = m_sampler;
    imgInfo.imageView   = ssaoView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_descSet;
    write.dstBinding      = 2;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
}

void PostProcess::updateDepthDescriptor(VkImageView depthView) {
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = m_sampler;
    imgInfo.imageView   = depthView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_descSet;
    write.dstBinding      = 3;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;

    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
}

} // namespace glory
