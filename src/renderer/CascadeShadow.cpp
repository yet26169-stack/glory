#include "renderer/CascadeShadow.h"
#include "renderer/Buffer.h"
#include "renderer/Device.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace glory {

namespace {
std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("CascadeShadow: cannot open " + path);
    size_t sz = static_cast<size_t>(f.tellg());
    std::vector<char> buf(sz);
    f.seekg(0); f.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
}
VkShaderModule makeModule(VkDevice dev, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m), "CascadeShadow shader");
    return m;
}
} // namespace

// ── Static initialiser ────────────────────────────────────────────────────
constexpr float CascadeShadow::CASCADE_SPLITS[CASCADE_COUNT];

CascadeShadow::CascadeShadow(const Device& device) { init(device); }

CascadeShadow::~CascadeShadow() { destroy(); }

void CascadeShadow::init(const Device& device) {
    m_device = &device;
    createDepthArray();
    createRenderPass();
    createFramebuffers();
    createSampler();
    createUBO();
    createDescriptors();
    createPipeline();
    m_initialised = true;
    spdlog::info("CascadeShadow: {} cascades, {}x{} each", CASCADE_COUNT,
                 SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
}

// ── Depth image array ─────────────────────────────────────────────────────
void CascadeShadow::createDepthArray() {
    VkImageCreateInfo imgCI{};
    imgCI.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType   = VK_IMAGE_TYPE_2D;
    imgCI.format      = VK_FORMAT_D32_SFLOAT;
    imgCI.extent      = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1};
    imgCI.mipLevels   = 1;
    imgCI.arrayLayers = CASCADE_COUNT;
    imgCI.samples     = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VK_CHECK(vmaCreateImage(m_device->getAllocator(), &imgCI, &allocCI,
                            &m_depthImage, &m_depthAlloc, nullptr),
             "CascadeShadow depth array");

    // Full-array view (for binding to scene shader)
    VkImageViewCreateInfo avCI{};
    avCI.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    avCI.image      = m_depthImage;
    avCI.viewType   = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    avCI.format     = VK_FORMAT_D32_SFLOAT;
    avCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    avCI.subresourceRange.levelCount     = 1;
    avCI.subresourceRange.baseArrayLayer = 0;
    avCI.subresourceRange.layerCount     = CASCADE_COUNT;
    VK_CHECK(vkCreateImageView(m_device->getDevice(), &avCI, nullptr, &m_arrayView),
             "CascadeShadow array view");

    // Per-layer views (for framebuffer attachments)
    for (uint32_t i = 0; i < CASCADE_COUNT; ++i) {
        VkImageViewCreateInfo lvCI = avCI;
        lvCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        lvCI.subresourceRange.baseArrayLayer = i;
        lvCI.subresourceRange.layerCount     = 1;
        VK_CHECK(vkCreateImageView(m_device->getDevice(), &lvCI, nullptr,
                                   &m_layerViews[i]),
                 "CascadeShadow layer view");
    }

    // Transition all layers from UNDEFINED → DEPTH_STENCIL_READ_ONLY_OPTIMAL
    {
        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo cbAI{};
        cbAI.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbAI.commandPool        = m_device->getGraphicsCommandPool();
        cbAI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAI.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device->getDevice(), &cbAI, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask       = 0;
        barrier.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = m_depthImage;
        barrier.subresourceRange    = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, CASCADE_COUNT};

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submitInfo{};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &cmd;
        vkQueueSubmit(m_device->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_device->getGraphicsQueue());

        vkFreeCommandBuffers(m_device->getDevice(), m_device->getGraphicsCommandPool(), 1, &cmd);
    }
}

// ── Render pass ───────────────────────────────────────────────────────────
void CascadeShadow::createRenderPass() {
    VkAttachmentDescription att{};
    att.format         = VK_FORMAT_D32_SFLOAT;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription  sub{};
    sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &ref;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = 1;
    rpCI.pAttachments    = &att;
    rpCI.subpassCount    = 1;
    rpCI.pSubpasses      = &sub;
    rpCI.dependencyCount = 2;
    rpCI.pDependencies   = deps;
    VK_CHECK(vkCreateRenderPass(m_device->getDevice(), &rpCI, nullptr, &m_renderPass),
             "CascadeShadow render pass");
}

void CascadeShadow::createFramebuffers() {
    for (uint32_t i = 0; i < CASCADE_COUNT; ++i) {
        VkFramebufferCreateInfo fbCI{};
        fbCI.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbCI.renderPass      = m_renderPass;
        fbCI.attachmentCount = 1;
        fbCI.pAttachments    = &m_layerViews[i];
        fbCI.width           = SHADOW_MAP_SIZE;
        fbCI.height          = SHADOW_MAP_SIZE;
        fbCI.layers          = 1;
        VK_CHECK(vkCreateFramebuffer(m_device->getDevice(), &fbCI, nullptr,
                                     &m_framebuffers[i]),
                 "CascadeShadow framebuffer");
    }
}

// ── Sampler ───────────────────────────────────────────────────────────────
void CascadeShadow::createSampler() {
    VkSamplerCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter        = VK_FILTER_LINEAR;
    ci.minFilter        = VK_FILTER_LINEAR;
    ci.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    ci.compareEnable    = VK_FALSE;
    ci.compareOp        = VK_COMPARE_OP_ALWAYS;
    VK_CHECK(vkCreateSampler(m_device->getDevice(), &ci, nullptr, &m_sampler),
             "CascadeShadow sampler");
}

// ── UBO ───────────────────────────────────────────────────────────────────
void CascadeShadow::createUBO() {
    VkDeviceSize sz = sizeof(glm::mat4) * CASCADE_COUNT;
    m_uboBuffer = Buffer(m_device->getAllocator(), sz,
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VMA_MEMORY_USAGE_CPU_TO_GPU);
}

// ── Descriptors ───────────────────────────────────────────────────────────
void CascadeShadow::createDescriptors() {
    // Layout: binding 0 = light VP UBO, binding 1 = shadow array sampler
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 2;
    lci.pBindings    = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device->getDevice(), &lci, nullptr,
                                         &m_descLayout),
             "CascadeShadow desc layout");

    VkDescriptorPoolSize sizes[2]{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    sizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets       = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes    = sizes;
    VK_CHECK(vkCreateDescriptorPool(m_device->getDevice(), &pci, nullptr, &m_descPool),
             "CascadeShadow desc pool");

    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = m_descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &m_descLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_device->getDevice(), &ai, &m_descSet),
             "CascadeShadow desc set");

    VkDescriptorBufferInfo bufInfo{m_uboBuffer.getBuffer(), 0, sizeof(glm::mat4) * CASCADE_COUNT};

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = m_sampler;
    imgInfo.imageView   = m_arrayView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_descSet,
                 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufInfo, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_descSet,
                 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imgInfo, nullptr, nullptr};
    vkUpdateDescriptorSets(m_device->getDevice(), 2, writes, 0, nullptr);
}

// ── Pipeline (reuses shadow.vert / shadow.frag) ───────────────────────────
void CascadeShadow::createPipeline() {
    VkDevice dev = m_device->getDevice();

    // Push constant: model matrix (64 bytes) at vertex stage
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &m_descLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &m_pipelineLayout),
             "CascadeShadow pipeline layout");

    auto vertCode = readFile(std::string(SHADER_DIR) + "shadow.vert.spv");
    auto fragCode = readFile(std::string(SHADER_DIR) + "shadow.frag.spv");
    VkShaderModule vertMod = makeModule(dev, vertCode);
    VkShaderModule fragMod = makeModule(dev, fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    auto bindingDesc = Vertex::getBindingDescription();
    auto attrDescs   = Vertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo viCI{};
    viCI.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viCI.vertexBindingDescriptionCount   = 1;
    viCI.pVertexBindingDescriptions      = &bindingDesc;
    viCI.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
    viCI.pVertexAttributeDescriptions    = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo iaCI{};
    iaCI.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iaCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpCI{};
    vpCI.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpCI.viewportCount = 1;
    vpCI.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rastCI{};
    rastCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rastCI.polygonMode            = VK_POLYGON_MODE_FILL;
    rastCI.lineWidth              = 1.0f;
    rastCI.cullMode               = VK_CULL_MODE_FRONT_BIT;
    rastCI.frontFace              = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rastCI.depthBiasEnable        = VK_TRUE;
    rastCI.depthBiasConstantFactor = 1.25f;
    rastCI.depthBiasSlopeFactor   = 1.75f;

    VkPipelineMultisampleStateCreateInfo msCI{};
    msCI.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsCI{};
    dsCI.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dsCI.depthTestEnable  = VK_TRUE;
    dsCI.depthWriteEnable = VK_TRUE;
    dsCI.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendStateCreateInfo cbCI{};
    cbCI.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbCI.attachmentCount = 0;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                   VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynCI{};
    dynCI.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynCI.dynamicStateCount = 3;
    dynCI.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo pCI{};
    pCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pCI.stageCount          = 2;
    pCI.pStages             = stages;
    pCI.pVertexInputState   = &viCI;
    pCI.pInputAssemblyState = &iaCI;
    pCI.pViewportState      = &vpCI;
    pCI.pRasterizationState = &rastCI;
    pCI.pMultisampleState   = &msCI;
    pCI.pDepthStencilState  = &dsCI;
    pCI.pColorBlendState    = &cbCI;
    pCI.pDynamicState       = &dynCI;
    pCI.layout              = m_pipelineLayout;
    pCI.renderPass          = m_renderPass;
    pCI.subpass             = 0;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pCI, nullptr, &m_pipeline),
             "CascadeShadow pipeline");

    vkDestroyShaderModule(dev, vertMod, nullptr);
    vkDestroyShaderModule(dev, fragMod, nullptr);
}

std::array<glm::mat4, CascadeShadow::CASCADE_COUNT> CascadeShadow::computeCascadeVPs(
    const glm::mat4& view, float fov, float aspect, 
    float nearPlane, float farPlane, const glm::vec3& lightDir) 
{
    std::array<glm::mat4, CASCADE_COUNT> vps;
    glm::mat4 invCam = glm::inverse(glm::perspective(fov, aspect, nearPlane, farPlane) * view);

    float lastSplit = 0.0f;
    for (uint32_t i = 0; i < CASCADE_COUNT; ++i) {
        float split = CASCADE_SPLITS[i];
        
        // Calculate frustum corners in world space for this cascade
        std::vector<glm::vec4> corners;
        for (uint32_t x = 0; x < 2; ++x) {
            for (uint32_t y = 0; y < 2; ++y) {
                for (uint32_t z = 0; z < 2; ++z) {
                    glm::vec4 pt(2.0f * x - 1.0f, 2.0f * y - 1.0f, z, 1.0f);
                    glm::vec4 wPt = invCam * pt;
                    corners.push_back(wPt / wPt.w);
                }
            }
        }

        // Center of the frustum segment
        glm::vec3 center(0.0f);
        for (const auto& v : corners) center += glm::vec3(v);
        center /= static_cast<float>(corners.size());

        float radius = 0.0f;
        for (const auto& v : corners) radius = std::max(radius, glm::distance(center, glm::vec3(v)));
        radius = std::ceil(radius * 16.0f) / 16.0f;

        glm::vec3 maxExtents(radius);
        glm::vec3 minExtents(-radius);

        glm::mat4 lightView = glm::lookAt(center - lightDir * radius, center, glm::vec3(0, 1, 0));
        glm::mat4 lightProj = glm::ortho(minExtents.x, maxExtents.x, minExtents.y, maxExtents.y, -10.0f, 2.0f * radius);

        vps[i] = lightProj * lightView;
        lastSplit = split;
    }

    return vps;
}

// ── Update UBO ────────────────────────────────────────────────────────────
void CascadeShadow::updateLightMatrices(
    const std::array<glm::mat4, CASCADE_COUNT>& vps) {
    std::memcpy(m_uboBuffer.map(), vps.data(), sizeof(glm::mat4) * CASCADE_COUNT);
}

// ... computeCascadeVPs stays exactly same ...

// ── Destroy ───────────────────────────────────────────────────────────────
void CascadeShadow::destroy() {
    if (!m_device) return;
    VkDevice dev = m_device->getDevice();

    if (m_pipeline)   vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    for (auto fb : m_framebuffers) if (fb) vkDestroyFramebuffer(dev, fb, nullptr);
    if (m_renderPass)  vkDestroyRenderPass(dev, m_renderPass, nullptr);
    if (m_sampler)     vkDestroySampler(dev, m_sampler, nullptr);
    for (auto lv : m_layerViews) if (lv) vkDestroyImageView(dev, lv, nullptr);
    if (m_arrayView)   vkDestroyImageView(dev, m_arrayView, nullptr);
    if (m_depthImage)  vmaDestroyImage(m_device->getAllocator(), m_depthImage, m_depthAlloc);
    
    m_uboBuffer.destroy();

    if (m_descPool)    vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_descLayout)  vkDestroyDescriptorSetLayout(dev, m_descLayout, nullptr);

    m_pipeline = VK_NULL_HANDLE;
    m_initialised = false;
    m_device = nullptr;
}

} // namespace glory
