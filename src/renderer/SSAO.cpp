#include "renderer/SSAO.h"
#include "renderer/Buffer.h"
#include "renderer/Device.h"
#include "renderer/Swapchain.h"
#include "renderer/VkCheck.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>

namespace glory {

namespace {
std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Failed to open shader: " + path);
    size_t sz = static_cast<size_t>(file.tellg());
    std::vector<char> buf(sz);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
}

VkShaderModule makeModule(VkDevice dev, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod), "shader module");
    return mod;
}
} // anonymous namespace

SSAO::SSAO(const Device& device, const Swapchain& swapchain, VkImageView depthView)
    : m_device(device)
{
    m_width  = swapchain.getExtent().width  / 2;
    m_height = swapchain.getExtent().height / 2;

    createSamplers();
    createImages(m_width, m_height);
    createNoiseTexture();
    createRenderPass();
    createFramebuffers();
    createDescriptors(depthView);
    createPipelines();
    spdlog::info("SSAO pipeline created ({}x{})", m_width, m_height);
}

SSAO::~SSAO() { cleanup(); }

void SSAO::cleanup() {
    if (m_cleaned) return;
    m_cleaned = true;

    VkDevice dev = m_device.getDevice();
    destroyResizableResources();

    if (m_ssaoPipeline)       vkDestroyPipeline(dev, m_ssaoPipeline, nullptr);
    if (m_pipelineLayout)     vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_blurPipeline)       vkDestroyPipeline(dev, m_blurPipeline, nullptr);
    if (m_blurPipelineLayout) vkDestroyPipelineLayout(dev, m_blurPipelineLayout, nullptr);
    if (m_renderPass)         vkDestroyRenderPass(dev, m_renderPass, nullptr);
    if (m_descPool)           vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_ssaoDescLayout)     vkDestroyDescriptorSetLayout(dev, m_ssaoDescLayout, nullptr);
    if (m_blurDescLayout)     vkDestroyDescriptorSetLayout(dev, m_blurDescLayout, nullptr);
    if (m_sampler)            vkDestroySampler(dev, m_sampler, nullptr);
    if (m_noiseSampler)       vkDestroySampler(dev, m_noiseSampler, nullptr);
    m_noiseImage = Image{};
}

void SSAO::destroyResizableResources() {
    VkDevice dev = m_device.getDevice();
    if (m_aoFB)   vkDestroyFramebuffer(dev, m_aoFB, nullptr);
    if (m_blurFB) vkDestroyFramebuffer(dev, m_blurFB, nullptr);
    m_aoFB = VK_NULL_HANDLE;
    m_blurFB = VK_NULL_HANDLE;
    m_aoImage = Image{};
    m_blurImage = Image{};
}

void SSAO::recreate(const Swapchain& swapchain, VkImageView depthView) {
    vkDeviceWaitIdle(m_device.getDevice());
    destroyResizableResources();
    m_width  = swapchain.getExtent().width  / 2;
    m_height = swapchain.getExtent().height / 2;
    createImages(m_width, m_height);
    createFramebuffers();

    // Update depth descriptor
    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler     = m_sampler;
    depthInfo.imageView   = depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_ssaoDescSet;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &depthInfo;
    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);

    // Update blur input descriptor
    VkDescriptorImageInfo aoInfo{};
    aoInfo.sampler     = m_sampler;
    aoInfo.imageView   = m_aoImage.getImageView();
    aoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    write.dstSet    = m_blurDescSet;
    write.dstBinding = 0;
    write.pImageInfo = &aoInfo;
    vkUpdateDescriptorSets(m_device.getDevice(), 1, &write, 0, nullptr);
}

void SSAO::createImages(uint32_t w, uint32_t h) {
    m_aoImage = Image(m_device, w, h,
                      VK_FORMAT_R8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT);
    m_blurImage = Image(m_device, w, h,
                        VK_FORMAT_R8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
}

void SSAO::createNoiseTexture() {
    // 4x4 random rotation vectors (tangent-space)
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    uint32_t pixels[16];
    for (int i = 0; i < 16; ++i) {
        float x = dist(rng);
        float y = dist(rng);
        // Encode to [0,255]: map [-1,1] → [0,255]
        uint8_t r = static_cast<uint8_t>((x * 0.5f + 0.5f) * 255.0f);
        uint8_t g = static_cast<uint8_t>((y * 0.5f + 0.5f) * 255.0f);
        uint8_t b = 128; // z = 0
        pixels[i] = static_cast<uint32_t>(r) |
                    (static_cast<uint32_t>(g) << 8) |
                    (static_cast<uint32_t>(b) << 16) |
                    (0xFFu << 24);
    }

    VkDeviceSize size = 4 * 4 * 4;
    Buffer staging(m_device.getAllocator(), size,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    void* mapped = staging.map();
    std::memcpy(mapped, pixels, static_cast<size_t>(size));
    staging.unmap();

    m_noiseImage = Image(m_device, 4, 4,
                         VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);

    // Transition + copy (single-use command buffer)
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool        = m_device.getTransferCommandPool();
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device.getDevice(), &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = m_noiseImage.getImage();
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask       = 0;
    barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {4, 4, 1};
    vkCmdCopyBufferToImage(cmd, staging.getBuffer(), m_noiseImage.getImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;
    vkQueueSubmit(m_device.getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_device.getGraphicsQueue());
    vkFreeCommandBuffers(m_device.getDevice(), m_device.getTransferCommandPool(), 1, &cmd);
}

void SSAO::createRenderPass() {
    VkAttachmentDescription attachment{};
    attachment.format         = VK_FORMAT_R8_UNORM;
    attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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
    ci.pAttachments    = &attachment;
    ci.subpassCount    = 1;
    ci.pSubpasses      = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    VK_CHECK(vkCreateRenderPass(m_device.getDevice(), &ci, nullptr, &m_renderPass),
             "Failed to create SSAO render pass");
}

void SSAO::createFramebuffers() {
    VkDevice dev = m_device.getDevice();

    VkImageView aoView = m_aoImage.getImageView();
    VkFramebufferCreateInfo ci{};
    ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass      = m_renderPass;
    ci.attachmentCount = 1;
    ci.pAttachments    = &aoView;
    ci.width           = m_width;
    ci.height          = m_height;
    ci.layers          = 1;
    VK_CHECK(vkCreateFramebuffer(dev, &ci, nullptr, &m_aoFB), "SSAO FB");

    VkImageView blurView = m_blurImage.getImageView();
    ci.pAttachments = &blurView;
    VK_CHECK(vkCreateFramebuffer(dev, &ci, nullptr, &m_blurFB), "SSAO blur FB");
}

void SSAO::createSamplers() {
    VkDevice dev = m_device.getDevice();

    // Clamp-to-edge sampler for AO/depth
    VkSamplerCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter    = VK_FILTER_LINEAR;
    ci.minFilter    = VK_FILTER_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(dev, &ci, nullptr, &m_sampler), "SSAO sampler");

    // Repeat sampler for noise (tiled)
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.magFilter    = VK_FILTER_NEAREST;
    ci.minFilter    = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(dev, &ci, nullptr, &m_noiseSampler), "SSAO noise sampler");
}

void SSAO::createDescriptors(VkImageView depthView) {
    VkDevice dev = m_device.getDevice();

    // SSAO layout: binding 0 = depth, binding 1 = noise
    std::array<VkDescriptorSetLayoutBinding, 2> ssaoBindings{};
    ssaoBindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    ssaoBindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = static_cast<uint32_t>(ssaoBindings.size());
    layoutCI.pBindings    = ssaoBindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_ssaoDescLayout), "SSAO desc layout");

    // Blur layout: binding 0 = AO texture
    VkDescriptorSetLayoutBinding blurBinding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    layoutCI.bindingCount = 1;
    layoutCI.pBindings    = &blurBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr, &m_blurDescLayout), "SSAO blur desc layout");

    // Pool: 3 combined image samplers (2 for SSAO + 1 for blur)
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    poolCI.maxSets       = 2;
    VK_CHECK(vkCreateDescriptorPool(dev, &poolCI, nullptr, &m_descPool), "SSAO desc pool");

    // Allocate sets
    VkDescriptorSetLayout layouts[] = {m_ssaoDescLayout, m_blurDescLayout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts        = layouts;
    VK_CHECK(vkAllocateDescriptorSets(dev, &allocInfo, sets), "SSAO desc sets");
    m_ssaoDescSet = sets[0];
    m_blurDescSet = sets[1];

    // Write SSAO descriptors
    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler     = m_sampler;
    depthInfo.imageView   = depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo noiseInfo{};
    noiseInfo.sampler     = m_noiseSampler;
    noiseInfo.imageView   = m_noiseImage.getImageView();
    noiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_ssaoDescSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &depthInfo;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_ssaoDescSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &noiseInfo;
    vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    // Write blur descriptor
    VkDescriptorImageInfo aoInfo{};
    aoInfo.sampler     = m_sampler;
    aoInfo.imageView   = m_aoImage.getImageView();
    aoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet blurWrite{};
    blurWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    blurWrite.dstSet          = m_blurDescSet;
    blurWrite.dstBinding      = 0;
    blurWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    blurWrite.descriptorCount = 1;
    blurWrite.pImageInfo      = &aoInfo;
    vkUpdateDescriptorSets(dev, 1, &blurWrite, 0, nullptr);
}

void SSAO::createPipelines() {
    VkDevice dev = m_device.getDevice();

    auto vertCode     = readFile(std::string(SHADER_DIR) + "postprocess.vert.spv");
    auto ssaoFragCode = readFile(std::string(SHADER_DIR) + "ssao.frag.spv");
    auto blurFragCode = readFile(std::string(SHADER_DIR) + "ssao_blur.frag.spv");

    VkShaderModule vertMod     = makeModule(dev, vertCode);
    VkShaderModule ssaoFragMod = makeModule(dev, ssaoFragCode);
    VkShaderModule blurFragMod = makeModule(dev, blurFragCode);

    // Common state for fullscreen triangle
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dynStates;

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

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttach;

    // SSAO pipeline layout: push constant = 2 mat4 + 4 floats = 144 bytes
    // We use 128 bytes: 2 mat4s, and hardcode params in push constant
    VkPushConstantRange ssaoPush{};
    ssaoPush.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ssaoPush.offset     = 0;
    ssaoPush.size       = sizeof(float) * (16 + 16 + 4); // 144 bytes

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_ssaoDescLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &ssaoPush;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout), "SSAO pipeline layout");

    // Blur pipeline layout (no push constants)
    layoutCI.pSetLayouts            = &m_blurDescLayout;
    layoutCI.pushConstantRangeCount = 0;
    VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_blurPipelineLayout), "SSAO blur pipeline layout");

    // SSAO pipeline
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ssaoFragMod;
    stages[1].pName  = "main";

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.stageCount          = 2;
    pipelineCI.pStages             = stages;
    pipelineCI.pVertexInputState   = &vertexInput;
    pipelineCI.pInputAssemblyState = &inputAssembly;
    pipelineCI.pViewportState      = &viewportState;
    pipelineCI.pRasterizationState = &rasterizer;
    pipelineCI.pMultisampleState   = &multisample;
    pipelineCI.pDepthStencilState  = &depthStencil;
    pipelineCI.pColorBlendState    = &colorBlend;
    pipelineCI.pDynamicState       = &dynState;
    pipelineCI.layout              = m_pipelineLayout;
    pipelineCI.renderPass          = m_renderPass;
    pipelineCI.subpass             = 0;

    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &m_ssaoPipeline),
             "SSAO pipeline");

    // Blur pipeline
    stages[1].module = blurFragMod;
    pipelineCI.layout = m_blurPipelineLayout;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &m_blurPipeline),
             "SSAO blur pipeline");

    vkDestroyShaderModule(dev, blurFragMod, nullptr);
    vkDestroyShaderModule(dev, ssaoFragMod, nullptr);
    vkDestroyShaderModule(dev, vertMod, nullptr);
}

void SSAO::record(VkCommandBuffer cmd, const glm::mat4& proj, const glm::mat4& invProj,
                  float radius, float bias, float intensity) {
    VkViewport vp{};
    vp.width    = static_cast<float>(m_width);
    vp.height   = static_cast<float>(m_height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.extent = {m_width, m_height};

    VkClearValue clear{};
    clear.color = {{1.0f, 1.0f, 1.0f, 1.0f}};

    // Pass 1: SSAO computation
    {
        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass        = m_renderPass;
        rpInfo.framebuffer       = m_aoFB;
        rpInfo.renderArea.extent = {m_width, m_height};
        rpInfo.clearValueCount   = 1;
        rpInfo.pClearValues      = &clear;

        vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssaoPipeline);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        struct SSAOPushData {
            glm::mat4 projection;
            glm::mat4 invProjection;
            float radius;
            float bias;
            float intensity;
            float _pad;
        } pushData;
        pushData.projection    = proj;
        pushData.invProjection = invProj;
        pushData.radius        = radius;
        pushData.bias          = bias;
        pushData.intensity     = intensity;
        pushData._pad          = 0.0f;

        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pushData), &pushData);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipelineLayout, 0, 1, &m_ssaoDescSet, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // Pass 2: Blur
    {
        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass        = m_renderPass;
        rpInfo.framebuffer       = m_blurFB;
        rpInfo.renderArea.extent = {m_width, m_height};
        rpInfo.clearValueCount   = 1;
        rpInfo.pClearValues      = &clear;

        vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_blurPipeline);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_blurPipelineLayout, 0, 1, &m_blurDescSet, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }
}

} // namespace glory
