#include "fog/FogSystem.h"
#include "renderer/Device.h"

#include <spdlog/spdlog.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace glory {

// ── Destructor / Cleanup ────────────────────────────────────────────────────

FogSystem::~FogSystem() { cleanup(); }

void FogSystem::cleanup() {
  if (!m_initialized || !m_device)
    return;
  VkDevice vkDev = m_device->getDevice();
  VmaAllocator allocator = m_device->getAllocator();
  vkDeviceWaitIdle(vkDev);

  auto destroyImg = [&](VkImage &img, void *&alloc, VkImageView &view) {
    if (view) {
      vkDestroyImageView(vkDev, view, nullptr);
      view = VK_NULL_HANDLE;
    }
    if (img) {
      vmaDestroyImage(allocator, img, static_cast<VmaAllocation>(alloc));
      img = VK_NULL_HANDLE;
      alloc = nullptr;
    }
  };

  destroyImg(m_visionImage, m_visionAlloc, m_visionView);
  destroyImg(m_explorationImage, m_explorationAlloc, m_explorationView);

  if (m_visionSampler) {
    vkDestroySampler(vkDev, m_visionSampler, nullptr);
    m_visionSampler = VK_NULL_HANDLE;
  }
  if (m_explorationSampler) {
    vkDestroySampler(vkDev, m_explorationSampler, nullptr);
    m_explorationSampler = VK_NULL_HANDLE;
  }

  if (m_stagingBuffer) {
    vmaDestroyBuffer(allocator, m_stagingBuffer,
                     static_cast<VmaAllocation>(m_stagingAlloc));
    m_stagingBuffer = VK_NULL_HANDLE;
  }
  if (m_uboBuffer) {
    vmaDestroyBuffer(allocator, m_uboBuffer,
                     static_cast<VmaAllocation>(m_uboAlloc));
    m_uboBuffer = VK_NULL_HANDLE;
  }

  if (m_pipeline) {
    vkDestroyPipeline(vkDev, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;
  }
  if (m_pipelineLayout) {
    vkDestroyPipelineLayout(vkDev, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
  }
  if (m_descPool) {
    vkDestroyDescriptorPool(vkDev, m_descPool, nullptr);
    m_descPool = VK_NULL_HANDLE;
  }
  if (m_descLayout) {
    vkDestroyDescriptorSetLayout(vkDev, m_descLayout, nullptr);
    m_descLayout = VK_NULL_HANDLE;
  }

  m_initialized = false;
}

// ── CPU Vision Painting ─────────────────────────────────────────────────────

static float smoothstepF(float edge0, float edge1, float x) {
  float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

void FogSystem::paintVisionCircle(float worldX, float worldZ, float radius) {
  float mapX = (worldX / WORLD_SIZE) * MAP_SIZE;
  float mapZ = (worldZ / WORLD_SIZE) * MAP_SIZE;
  float radiusPx = (radius / WORLD_SIZE) * MAP_SIZE;

  int minX = std::max(0, static_cast<int>(mapX - radiusPx) - 1);
  int maxX = std::min(MAP_SIZE - 1, static_cast<int>(mapX + radiusPx) + 1);
  int minZ = std::max(0, static_cast<int>(mapZ - radiusPx) - 1);
  int maxZ = std::min(MAP_SIZE - 1, static_cast<int>(mapZ + radiusPx) + 1);

  for (int z = minZ; z <= maxZ; ++z) {
    for (int x = minX; x <= maxX; ++x) {
      float dx = static_cast<float>(x) - mapX;
      float dz = static_cast<float>(z) - mapZ;
      float dist = std::sqrt(dx * dx + dz * dz);

      if (dist <= radiusPx) {
        float falloff = 1.0f - smoothstepF(radiusPx * 0.8f, radiusPx, dist);
        uint8_t val = static_cast<uint8_t>(falloff * 255.0f);
        m_visionBuffer[z * MAP_SIZE + x] =
            std::max(m_visionBuffer[z * MAP_SIZE + x], val);
      }
    }
  }
}

void FogSystem::updateCpuOnly(const std::vector<VisionEntity> &entities) {
  // Clear vision (exploration is persistent)
  std::fill(m_visionBuffer.begin(), m_visionBuffer.end(), uint8_t(0));

  for (const auto &e : entities) {
    paintVisionCircle(e.position.x, e.position.z, e.sightRange);
  }

  // Merge vision into exploration
  for (size_t i = 0; i < m_explorationBuffer.size(); ++i) {
    m_explorationBuffer[i] =
        std::max(m_explorationBuffer[i], m_visionBuffer[i]);
  }
}

void FogSystem::update(const std::vector<VisionEntity> &entities) {
  updateCpuOnly(entities);
  if (m_initialized)
    uploadTextures();
}

// ── CPU Queries ─────────────────────────────────────────────────────────────

bool FogSystem::isPositionVisible(float worldX, float worldZ) const {
  int px = static_cast<int>((worldX / WORLD_SIZE) * MAP_SIZE);
  int pz = static_cast<int>((worldZ / WORLD_SIZE) * MAP_SIZE);
  px = std::clamp(px, 0, MAP_SIZE - 1);
  pz = std::clamp(pz, 0, MAP_SIZE - 1);
  return m_visionBuffer[pz * MAP_SIZE + px] > 25;
}

bool FogSystem::isPositionExplored(float worldX, float worldZ) const {
  int px = static_cast<int>((worldX / WORLD_SIZE) * MAP_SIZE);
  int pz = static_cast<int>((worldZ / WORLD_SIZE) * MAP_SIZE);
  px = std::clamp(px, 0, MAP_SIZE - 1);
  pz = std::clamp(pz, 0, MAP_SIZE - 1);
  return m_explorationBuffer[pz * MAP_SIZE + px] > 25;
}

// ── GPU Texture Upload ──────────────────────────────────────────────────────

void FogSystem::uploadTextures() {
  if (!m_device)
    return;

  VkDeviceSize texSize = MAP_SIZE * MAP_SIZE;
  // Copy both vision + exploration into staging (side by side)
  uint8_t *dst = static_cast<uint8_t *>(m_stagingMapped);
  std::memcpy(dst, m_visionBuffer.data(), texSize);
  std::memcpy(dst + texSize, m_explorationBuffer.data(), texSize);

  VkCommandBuffer cmd;
  VkCommandBufferAllocateInfo allocInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocInfo.commandPool = m_device->getTransferCommandPool();
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;
  vkAllocateCommandBuffers(m_device->getDevice(), &allocInfo, &cmd);

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);

  // Transition both images to TRANSFER_DST
  VkImageMemoryBarrier barriers[2]{};
  for (int i = 0; i < 2; ++i) {
    barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[i].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barriers[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  }
  barriers[0].image = m_visionImage;
  barriers[1].image = m_explorationImage;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 2, barriers);

  // Copy from staging
  VkBufferImageCopy regions[2]{};
  for (int i = 0; i < 2; ++i) {
    regions[i].bufferOffset = i * texSize;
    regions[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    regions[i].imageExtent = {MAP_SIZE, MAP_SIZE, 1};
  }
  vkCmdCopyBufferToImage(cmd, m_stagingBuffer, m_visionImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &regions[0]);
  vkCmdCopyBufferToImage(cmd, m_stagingBuffer, m_explorationImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &regions[1]);

  // Transition back to SHADER_READ
  for (int i = 0; i < 2; ++i) {
    barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  }
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 2, barriers);

  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(m_device->getGraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(m_device->getGraphicsQueue());
  vkFreeCommandBuffers(m_device->getDevice(),
                       m_device->getTransferCommandPool(), 1, &cmd);
}

// ── Render (Post-Process) ───────────────────────────────────────────────────

void FogSystem::render(VkCommandBuffer cmd, const glm::mat4 &invViewProj) {
  if (!m_initialized)
    return;

  // Upload UBO
  FogUBO ubo{};
  ubo.invViewProj = invViewProj;
  ubo.fogColorExplored = glm::vec4(0.0f, 0.0f, 0.0f, 0.5f);
  ubo.fogColorUnknown = glm::vec4(0.0f, 0.0f, 0.05f, 0.95f);
  std::memcpy(m_uboMapped, &ubo, sizeof(FogUBO));

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);
  // Full-screen triangle — 3 vertices, no vertex buffer
  vkCmdDraw(cmd, 3, 1, 0, 0);
}

// ── Vulkan Resource Creation ────────────────────────────────────────────────

void FogSystem::createTextures(const Device &device) {
  VkDevice vkDev = device.getDevice();
  VmaAllocator allocator = device.getAllocator();

  auto createR8Texture = [&](VkImage &img, void *&alloc, VkImageView &view,
                             VkSampler &sampler) {
    VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8_UNORM;
    imgInfo.extent = {MAP_SIZE, MAP_SIZE, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VmaAllocation vmaAlloc;
    vmaCreateImage(allocator, &imgInfo, &allocCI, &img, &vmaAlloc, nullptr);
    alloc = vmaAlloc;

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = img;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(vkDev, &viewInfo, nullptr, &view);

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    vkCreateSampler(vkDev, &samplerInfo, nullptr, &sampler);

    // Transition to SHADER_READ_ONLY initially
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmdInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = device.getTransferCommandPool();
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(vkDev, &cmdInfo, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cmd;
    vkQueueSubmit(device.getGraphicsQueue(), 1, &sub, VK_NULL_HANDLE);
    vkQueueWaitIdle(device.getGraphicsQueue());
    vkFreeCommandBuffers(vkDev, device.getTransferCommandPool(), 1, &cmd);
  };

  createR8Texture(m_visionImage, m_visionAlloc, m_visionView, m_visionSampler);
  createR8Texture(m_explorationImage, m_explorationAlloc, m_explorationView,
                  m_explorationSampler);

  // Staging buffer for both textures (2 * 128*128 bytes)
  VkDeviceSize stagingSize = MAP_SIZE * MAP_SIZE * 2;
  VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufInfo.size = stagingSize;
  bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo bufAI{};
  bufAI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
  bufAI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo mapInfo{};
  VmaAllocation stgAlloc;
  vmaCreateBuffer(allocator, &bufInfo, &bufAI, &m_stagingBuffer, &stgAlloc,
                  &mapInfo);
  m_stagingAlloc = stgAlloc;
  m_stagingMapped = mapInfo.pMappedData;

  // UBO buffer
  VkBufferCreateInfo uboBufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  uboBufInfo.size = sizeof(FogUBO);
  uboBufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  VmaAllocationCreateInfo uboAI{};
  uboAI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
  uboAI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo uboMapInfo{};
  VmaAllocation uboA;
  vmaCreateBuffer(allocator, &uboBufInfo, &uboAI, &m_uboBuffer, &uboA,
                  &uboMapInfo);
  m_uboAlloc = uboA;
  m_uboMapped = uboMapInfo.pMappedData;

  spdlog::info("FogSystem textures created ({}x{} R8 UNORM)", MAP_SIZE,
               MAP_SIZE);
}

std::vector<char> FogSystem::readFile(const std::string &filepath) {
  std::ifstream file(filepath, std::ios::ate | std::ios::binary);
  if (!file.is_open())
    throw std::runtime_error("FogSystem: cannot open shader: " + filepath);
  size_t fileSize = static_cast<size_t>(file.tellg());
  std::vector<char> buffer(fileSize);
  file.seekg(0);
  file.read(buffer.data(), fileSize);
  return buffer;
}

VkShaderModule
FogSystem::createShaderModule(VkDevice device,
                              const std::string &filepath) const {
  auto code = readFile(filepath);
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = code.size();
  info.pCode = reinterpret_cast<const uint32_t *>(code.data());
  VkShaderModule module;
  if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
    throw std::runtime_error("FogSystem: failed to create shader module");
  return module;
}

void FogSystem::createDescriptors(const Device &device,
                                  VkImageView sceneColorView,
                                  VkSampler sceneColorSampler,
                                  VkImageView sceneDepthView,
                                  VkSampler sceneDepthSampler) {
  VkDevice vkDev = device.getDevice();

  // Layout: 5 bindings (sceneColor, sceneDepth, visionMap, explorationMap,
  // fogUBO)
  VkDescriptorSetLayoutBinding bindings[5]{};
  for (int i = 0; i < 4; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  bindings[4].binding = 4;
  bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  bindings[4].descriptorCount = 1;
  bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo layoutCI{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layoutCI.bindingCount = 5;
  layoutCI.pBindings = bindings;
  vkCreateDescriptorSetLayout(vkDev, &layoutCI, nullptr, &m_descLayout);

  // Pool
  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
  VkDescriptorPoolCreateInfo poolCI{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolCI.maxSets = 1;
  poolCI.poolSizeCount = 2;
  poolCI.pPoolSizes = poolSizes;
  vkCreateDescriptorPool(vkDev, &poolCI, nullptr, &m_descPool);

  // Allocate set
  VkDescriptorSetAllocateInfo setInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  setInfo.descriptorPool = m_descPool;
  setInfo.descriptorSetCount = 1;
  setInfo.pSetLayouts = &m_descLayout;
  vkAllocateDescriptorSets(vkDev, &setInfo, &m_descSet);

  // Write descriptors
  VkDescriptorImageInfo imgInfos[4] = {
      {sceneColorSampler, sceneColorView,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {sceneDepthSampler, sceneDepthView,
       VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
      {m_visionSampler, m_visionView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {m_explorationSampler, m_explorationView,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  VkDescriptorBufferInfo bufInfo{m_uboBuffer, 0, sizeof(FogUBO)};

  VkWriteDescriptorSet writes[5]{};
  for (int i = 0; i < 4; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = m_descSet;
    writes[i].dstBinding = i;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].descriptorCount = 1;
    writes[i].pImageInfo = &imgInfos[i];
  }
  writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[4].dstSet = m_descSet;
  writes[4].dstBinding = 4;
  writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[4].descriptorCount = 1;
  writes[4].pBufferInfo = &bufInfo;
  vkUpdateDescriptorSets(vkDev, 5, writes, 0, nullptr);
}

void FogSystem::createPipeline(const Device &device, VkRenderPass renderPass) {
  VkDevice vkDev = device.getDevice();
  std::string shaderDir = SHADER_DIR;

  VkShaderModule vertModule =
      createShaderModule(vkDev, shaderDir + "fog.vert.spv");
  VkShaderModule fragModule =
      createShaderModule(vkDev, shaderDir + "fog.frag.spv");

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               nullptr,
               0,
               VK_SHADER_STAGE_VERTEX_BIT,
               vertModule,
               "main"};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               nullptr,
               0,
               VK_SHADER_STAGE_FRAGMENT_BIT,
               fragModule,
               "main"};

  // No vertex input — full-screen triangle from gl_VertexIndex
  VkPipelineVertexInputStateCreateInfo vertexInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynState{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynState.dynamicStateCount = 2;
  dynState.pDynamicStates = dynStates;

  VkPipelineViewportStateCreateInfo vpState{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vpState.viewportCount = 1;
  vpState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_NONE;

  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_FALSE; // post-process, no depth
  depth.depthWriteEnable = VK_FALSE;

  // Alpha blending (fog overlay)
  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo cb{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;

  VkPipelineLayoutCreateInfo layoutInfo{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &m_descLayout;
  vkCreatePipelineLayout(vkDev, &layoutInfo, nullptr, &m_pipelineLayout);

  VkGraphicsPipelineCreateInfo pipelineCI{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipelineCI.stageCount = 2;
  pipelineCI.pStages = stages;
  pipelineCI.pVertexInputState = &vertexInput;
  pipelineCI.pInputAssemblyState = &inputAssembly;
  pipelineCI.pViewportState = &vpState;
  pipelineCI.pRasterizationState = &rasterizer;
  pipelineCI.pMultisampleState = &ms;
  pipelineCI.pDepthStencilState = &depth;
  pipelineCI.pColorBlendState = &cb;
  pipelineCI.pDynamicState = &dynState;
  pipelineCI.layout = m_pipelineLayout;
  pipelineCI.renderPass = renderPass;

  vkCreateGraphicsPipelines(vkDev, VK_NULL_HANDLE, 1, &pipelineCI, nullptr,
                            &m_pipeline);

  vkDestroyShaderModule(vkDev, vertModule, nullptr);
  vkDestroyShaderModule(vkDev, fragModule, nullptr);
  spdlog::info("Fog pipeline created (post-process)");
}

// ── Initialization ──────────────────────────────────────────────────────────

void FogSystem::init(const Device &device, VkRenderPass renderPass,
                     VkImageView sceneColorView, VkSampler sceneColorSampler,
                     VkImageView sceneDepthView, VkSampler sceneDepthSampler) {
  m_device = &device;

  m_visionBuffer.resize(MAP_SIZE * MAP_SIZE, 0);
  m_explorationBuffer.resize(MAP_SIZE * MAP_SIZE, 0);

  createTextures(device);
  createDescriptors(device, sceneColorView, sceneColorSampler, sceneDepthView,
                    sceneDepthSampler);
  createPipeline(device, renderPass);

  m_initialized = true;
  spdlog::info("FogSystem initialized ({}x{} vision/exploration maps)",
               MAP_SIZE, MAP_SIZE);
}

} // namespace glory
