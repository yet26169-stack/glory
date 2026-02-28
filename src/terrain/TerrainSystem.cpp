#include "terrain/TerrainSystem.h"
#include "renderer/Buffer.h"
#include "renderer/Device.h"

#define STB_IMAGE_IMPLEMENTATION_GUARD
#include <spdlog/spdlog.h>
#include <stb_image.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace glory {

// ── Procedural noise helpers ────────────────────────────────────────────────

static float noise2D(int x, int y) {
  int n = x + y * 57;
  n = (n << 13) ^ n;
  return 1.0f - static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) &
                                   0x7FFFFFFF) /
                    1073741824.0f;
}

static float smoothstepF(float edge0, float edge1, float x) {
  float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static float smoothNoise(float x, float y) {
  int ix = static_cast<int>(std::floor(x));
  int iy = static_cast<int>(std::floor(y));
  float fx = smoothstepF(0, 1, x - ix);
  float fy = smoothstepF(0, 1, y - iy);
  float v00 = noise2D(ix, iy), v10 = noise2D(ix + 1, iy);
  float v01 = noise2D(ix, iy + 1), v11 = noise2D(ix + 1, iy + 1);
  return (v00 + (v10 - v00) * fx) +
         ((v01 + (v11 - v01) * fx) - (v00 + (v10 - v00) * fx)) * fy;
}

static float fbm(float x, float y, int octaves = 4) {
  float value = 0.0f, amplitude = 1.0f, frequency = 1.0f, maxAmp = 0.0f;
  for (int i = 0; i < octaves; ++i) {
    value += smoothNoise(x * frequency, y * frequency) * amplitude;
    maxAmp += amplitude;
    amplitude *= 0.5f;
    frequency *= 2.0f;
  }
  return value / maxAmp;
}

// ── Destructor / cleanup ────────────────────────────────────────────────────

TerrainSystem::~TerrainSystem() { cleanup(); }

void TerrainSystem::cleanup() {
  if (!m_initialized || !m_device)
    return;
  VkDevice vkDev = m_device->getDevice();
  VmaAllocator allocator = m_device->getAllocator();
  vkDeviceWaitIdle(vkDev);

  auto destroyPipeline = [&](VkPipeline &p) {
    if (p) {
      vkDestroyPipeline(vkDev, p, nullptr);
      p = VK_NULL_HANDLE;
    }
  };
  auto destroyLayout = [&](VkPipelineLayout &l) {
    if (l) {
      vkDestroyPipelineLayout(vkDev, l, nullptr);
      l = VK_NULL_HANDLE;
    }
  };
  auto destroyDSL = [&](VkDescriptorSetLayout &d) {
    if (d) {
      vkDestroyDescriptorSetLayout(vkDev, d, nullptr);
      d = VK_NULL_HANDLE;
    }
  };
  auto destroyBuf = [&](VkBuffer &b, void *&a) {
    if (b) {
      vmaDestroyBuffer(allocator, b, static_cast<VmaAllocation>(a));
      b = VK_NULL_HANDLE;
      a = nullptr;
    }
  };

  destroyPipeline(m_pipeline);
  destroyPipeline(m_wireframePipeline);
  destroyLayout(m_pipelineLayout);
  destroyPipeline(m_waterPipeline);
  destroyLayout(m_waterPipelineLayout);

  if (m_descPool) {
    vkDestroyDescriptorPool(vkDev, m_descPool, nullptr);
    m_descPool = VK_NULL_HANDLE;
  }
  destroyDSL(m_uboDescLayout);
  destroyDSL(m_texDescLayout);
  destroyDSL(m_waterTexDescLayout);

  destroyBuf(m_vbVma, m_vbAlloc);
  destroyBuf(m_ibVma, m_ibAlloc);
  destroyBuf(m_ubVma, m_ubAlloc);
  destroyBuf(m_waterVbVma, m_waterVbAlloc);
  destroyBuf(m_waterIbVma, m_waterIbAlloc);

  m_initialized = false;
}

// ── Heightmap generation / loading ──────────────────────────────────────────

void TerrainSystem::generateProceduralHeightmap() {
  m_heightmap.resize(m_hmSize * m_hmSize);
  for (int y = 0; y < m_hmSize; ++y) {
    for (int x = 0; x < m_hmSize; ++x) {
      float fx = static_cast<float>(x) / m_hmSize;
      float fy = static_cast<float>(y) / m_hmSize;
      float h = fbm(fx * 8.0f + 3.7f, fy * 8.0f + 1.3f, 5) * 0.5f + 0.5f;
      float riverDist = std::abs(fx - fy);
      h *= smoothstepF(0.0f, 0.08f, riverDist);
      float blueDist = std::sqrt(fx * fx + fy * fy);
      float redDist =
          std::sqrt((1.f - fx) * (1.f - fx) + (1.f - fy) * (1.f - fy));
      float bf = 1.0f - smoothstepF(0.12f, 0.2f, blueDist);
      float rf = 1.0f - smoothstepF(0.12f, 0.2f, redDist);
      h = h * (1.f - bf) + 0.15f * bf;
      h = h * (1.f - rf) + 0.15f * rf;
      m_heightmap[y * m_hmSize + x] = std::clamp(h, 0.0f, 1.0f);
    }
  }
  spdlog::info("Generated procedural {}x{} heightmap", m_hmSize, m_hmSize);
}

void TerrainSystem::loadHeightmap(const std::string &path) {
  int w, h, channels;
  unsigned char *data = stbi_load(path.c_str(), &w, &h, &channels, 1);
  if (!data) {
    spdlog::warn("Failed to load heightmap '{}', generating procedural", path);
    generateProceduralHeightmap();
    return;
  }
  m_hmSize = w;
  m_heightmap.resize(w * h);
  for (int i = 0; i < w * h; ++i)
    m_heightmap[i] = static_cast<float>(data[i]) / 255.0f;
  stbi_image_free(data);
  spdlog::info("Loaded heightmap '{}' ({}x{})", path, w, h);
}

// ── Mesh building ───────────────────────────────────────────────────────────

void TerrainSystem::buildMesh() {
  int vertsPerSide = m_hmSize;
  float step = m_worldSize / static_cast<float>(vertsPerSide - 1);

  m_totalVertices = vertsPerSide * vertsPerSide;
  std::vector<TerrainVertex> vertices(m_totalVertices);

  for (int z = 0; z < vertsPerSide; ++z) {
    for (int x = 0; x < vertsPerSide; ++x) {
      int idx = z * vertsPerSide + x;
      vertices[idx].pos =
          glm::vec3(x * step, m_heightmap[idx] * m_heightScale, z * step);
      vertices[idx].uv = glm::vec2(static_cast<float>(x) / (vertsPerSide - 1),
                                   static_cast<float>(z) / (vertsPerSide - 1));
    }
  }

  // Compute normals + tangents
  for (int z = 0; z < vertsPerSide; ++z) {
    for (int x = 0; x < vertsPerSide; ++x) {
      int idx = z * vertsPerSide + x;
      float hL =
          m_heightmap[z * vertsPerSide + std::max(x - 1, 0)] * m_heightScale;
      float hR =
          m_heightmap[z * vertsPerSide + std::min(x + 1, vertsPerSide - 1)] *
          m_heightScale;
      float hD =
          m_heightmap[std::max(z - 1, 0) * vertsPerSide + x] * m_heightScale;
      float hU =
          m_heightmap[std::min(z + 1, vertsPerSide - 1) * vertsPerSide + x] *
          m_heightScale;
      glm::vec3 normal =
          glm::normalize(glm::vec3(hL - hR, 2.0f * step, hD - hU));
      vertices[idx].normal = normal;
      glm::vec3 tangent =
          glm::normalize(glm::cross(glm::vec3(0, 1, 0), normal));
      if (glm::length(tangent) < 0.01f)
        tangent = glm::vec3(1, 0, 0);
      vertices[idx].tangent = tangent;
    }
  }

  // Generate indices + chunks
  int quadsPerSide = vertsPerSide - 1;
  int chunkQuads = quadsPerSide / m_chunksPerSide;
  m_chunks.clear();
  m_chunks.reserve(m_chunksPerSide * m_chunksPerSide);
  std::vector<uint32_t> allIndices;
  allIndices.reserve(quadsPerSide * quadsPerSide * 6);

  for (int cz = 0; cz < m_chunksPerSide; ++cz) {
    for (int cx = 0; cx < m_chunksPerSide; ++cx) {
      TerrainChunk chunk{};
      chunk.indexOffset = static_cast<uint32_t>(allIndices.size());
      int startX = cx * chunkQuads, startZ = cz * chunkQuads;
      int endX = std::min(startX + chunkQuads, quadsPerSide);
      int endZ = std::min(startZ + chunkQuads, quadsPerSide);
      glm::vec3 aabbMin(1e18f), aabbMax(-1e18f);

      for (int z = startZ; z < endZ; ++z) {
        for (int x = startX; x < endX; ++x) {
          uint32_t tl = z * vertsPerSide + x, tr = tl + 1;
          uint32_t bl = (z + 1) * vertsPerSide + x, br = bl + 1;
          allIndices.insert(allIndices.end(), {tl, bl, tr, tr, bl, br});
          for (uint32_t vi : {tl, tr, bl, br}) {
            aabbMin = glm::min(aabbMin, vertices[vi].pos);
            aabbMax = glm::max(aabbMax, vertices[vi].pos);
          }
          chunk.indexCount += 6;
        }
      }
      chunk.aabb.min = aabbMin;
      chunk.aabb.max = aabbMax;
      m_chunks.push_back(chunk);
    }
  }
  m_totalIndices = static_cast<uint32_t>(allIndices.size());
  spdlog::info("Terrain mesh: {} verts, {} indices, {} chunks", m_totalVertices,
               m_totalIndices, m_chunks.size());

  // Upload vertex buffer via staging
  VmaAllocator allocator = m_device->getAllocator();
  auto uploadBuffer = [&](const void *data, VkDeviceSize size,
                          VkBufferUsageFlags usage, VkBuffer &outBuf,
                          void *&outAlloc) {
    VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = size;
    bufInfo.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VmaAllocation alloc;
    vmaCreateBuffer(allocator, &bufInfo, &allocInfo, &outBuf, &alloc, nullptr);
    outAlloc = alloc;

    VkBuffer staging;
    VmaAllocation stgAlloc;
    VkBufferCreateInfo stgInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    stgInfo.size = size;
    stgInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo stgAllocInfo{};
    stgAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    vmaCreateBuffer(allocator, &stgInfo, &stgAllocInfo, &staging, &stgAlloc,
                    nullptr);

    void *mapped;
    vmaMapMemory(allocator, stgAlloc, &mapped);
    std::memcpy(mapped, data, size);
    vmaUnmapMemory(allocator, stgAlloc);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmdInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = m_device->getTransferCommandPool();
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(m_device->getDevice(), &cmdInfo, &cmd);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, staging, outBuf, 1, &copy);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_device->getGraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_device->getGraphicsQueue());
    vkFreeCommandBuffers(m_device->getDevice(),
                         m_device->getTransferCommandPool(), 1, &cmd);
    vmaDestroyBuffer(allocator, staging, stgAlloc);
  };

  uploadBuffer(vertices.data(), sizeof(TerrainVertex) * m_totalVertices,
               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_vbVma, m_vbAlloc);
  uploadBuffer(allIndices.data(), sizeof(uint32_t) * m_totalIndices,
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_ibVma, m_ibAlloc);
}

// ── Descriptors (set 0 = UBO, set 1 = textures) ────────────────────────────

void TerrainSystem::createDescriptors(const Device &device) {
  VkDevice vkDev = device.getDevice();
  VmaAllocator allocator = device.getAllocator();

  // UBO buffer (persistently mapped)
  {
    VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = sizeof(TerrainUBO);
    bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapInfo{};
    VmaAllocation alloc;
    vmaCreateBuffer(allocator, &bufInfo, &allocInfo, &m_ubVma, &alloc,
                    &mapInfo);
    m_ubAlloc = alloc;
    m_uboMapped = mapInfo.pMappedData;
  }

  // Set 0 layout: UBO
  {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    vkCreateDescriptorSetLayout(vkDev, &ci, nullptr, &m_uboDescLayout);
  }

  // Set 1 layout: 6 combined image samplers (terrain textures)
  {
    VkDescriptorSetLayoutBinding bindings[6]{};
    for (int i = 0; i < 6; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 6;
    ci.pBindings = bindings;
    vkCreateDescriptorSetLayout(vkDev, &ci, nullptr, &m_texDescLayout);
  }

  // Water texture set layout: 2 combined image samplers (splat + water normal)
  {
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 2;
    ci.pBindings = bindings;
    vkCreateDescriptorSetLayout(vkDev, &ci, nullptr, &m_waterTexDescLayout);
  }

  // Descriptor pool: 1 UBO + 6 terrain samplers + 2 water samplers = 9
  // descriptors, 4 sets
  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}};
  VkDescriptorPoolCreateInfo poolInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.maxSets = 4;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = poolSizes;
  vkCreateDescriptorPool(vkDev, &poolInfo, nullptr, &m_descPool);

  // Allocate descriptor sets
  VkDescriptorSetLayout layouts[] = {m_uboDescLayout, m_texDescLayout,
                                     m_waterTexDescLayout};
  VkDescriptorSet sets[3];
  VkDescriptorSetAllocateInfo setInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  setInfo.descriptorPool = m_descPool;
  setInfo.descriptorSetCount = 3;
  setInfo.pSetLayouts = layouts;
  vkAllocateDescriptorSets(vkDev, &setInfo, sets);
  m_uboDescSet = sets[0];
  m_texDescSet = sets[1];
  m_waterTexDescSet = sets[2];

  // Write UBO to set 0
  VkDescriptorBufferInfo uboBufInfo{m_ubVma, 0, sizeof(TerrainUBO)};
  VkWriteDescriptorSet uboWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  uboWrite.dstSet = m_uboDescSet;
  uboWrite.dstBinding = 0;
  uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uboWrite.descriptorCount = 1;
  uboWrite.pBufferInfo = &uboBufInfo;
  vkUpdateDescriptorSets(vkDev, 1, &uboWrite, 0, nullptr);

  // Write terrain textures to set 1
  auto imgInfo = [](const Texture &tex) -> VkDescriptorImageInfo {
    return {tex.getSampler(), tex.getImageView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  };
  VkDescriptorImageInfo texInfos[6] = {
      imgInfo(m_textures.grass),   imgInfo(m_textures.dirt),
      imgInfo(m_textures.stone),   imgInfo(m_textures.splatMap),
      imgInfo(m_textures.teamMap), imgInfo(m_textures.normalMap)};
  VkWriteDescriptorSet texWrites[6]{};
  for (int i = 0; i < 6; ++i) {
    texWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    texWrites[i].dstSet = m_texDescSet;
    texWrites[i].dstBinding = i;
    texWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texWrites[i].descriptorCount = 1;
    texWrites[i].pImageInfo = &texInfos[i];
  }
  vkUpdateDescriptorSets(vkDev, 6, texWrites, 0, nullptr);

  // Write water textures to water set (splat map + normal map as water normals)
  VkDescriptorImageInfo waterTexInfos[2] = {
      imgInfo(m_textures.splatMap),
      imgInfo(
          m_textures.normalMap) // reuse terrain normal map for water ripples
  };
  VkWriteDescriptorSet waterWrites[2]{};
  for (int i = 0; i < 2; ++i) {
    waterWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    waterWrites[i].dstSet = m_waterTexDescSet;
    waterWrites[i].dstBinding = i;
    waterWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    waterWrites[i].descriptorCount = 1;
    waterWrites[i].pImageInfo = &waterTexInfos[i];
  }
  vkUpdateDescriptorSets(vkDev, 2, waterWrites, 0, nullptr);
}

// ── Pipeline creation ───────────────────────────────────────────────────────

std::vector<char> TerrainSystem::readFile(const std::string &filepath) {
  std::ifstream file(filepath, std::ios::ate | std::ios::binary);
  if (!file.is_open())
    throw std::runtime_error("TerrainSystem: cannot open shader: " + filepath);
  size_t fileSize = static_cast<size_t>(file.tellg());
  std::vector<char> buffer(fileSize);
  file.seekg(0);
  file.read(buffer.data(), fileSize);
  return buffer;
}

VkShaderModule
TerrainSystem::createShaderModule(VkDevice device,
                                  const std::string &filepath) const {
  auto code = readFile(filepath);
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = code.size();
  info.pCode = reinterpret_cast<const uint32_t *>(code.data());
  VkShaderModule module;
  if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
    throw std::runtime_error("TerrainSystem: failed to create shader module");
  return module;
}

void TerrainSystem::createPipeline(const Device &device,
                                   VkRenderPass renderPass) {
  VkDevice vkDev = device.getDevice();
  std::string shaderDir = SHADER_DIR;

  // Terrain pipeline (set 0 = UBO, set 1 = textures)
  VkShaderModule vertModule =
      createShaderModule(vkDev, shaderDir + "terrain.vert.spv");
  VkShaderModule fragModule =
      createShaderModule(vkDev, shaderDir + "terrain.frag.spv");

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

  auto bindingDesc = TerrainVertex::getBindingDescription();
  auto attrDescs = TerrainVertex::getAttributeDescriptions();

  VkPipelineVertexInputStateCreateInfo vertexInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &bindingDesc;
  vertexInput.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(attrDescs.size());
  vertexInput.pVertexAttributeDescriptions = attrDescs.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynState{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynState.dynamicStateCount = 2;
  dynState.pDynamicStates = dynStates;

  VkPipelineViewportStateCreateInfo viewportState{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  VkPipelineMultisampleStateCreateInfo multisampling{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

  VkPipelineColorBlendAttachmentState colorBlendAttachment{};
  colorBlendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo colorBlending{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;

  // Pipeline layout: set 0 = UBO, set 1 = textures
  VkDescriptorSetLayout setLayouts[] = {m_uboDescLayout, m_texDescLayout};
  VkPipelineLayoutCreateInfo layoutInfo{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.setLayoutCount = 2;
  layoutInfo.pSetLayouts = setLayouts;
  vkCreatePipelineLayout(vkDev, &layoutInfo, nullptr, &m_pipelineLayout);

  VkGraphicsPipelineCreateInfo pipelineInfo{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDynamicState = &dynState;
  pipelineInfo.layout = m_pipelineLayout;
  pipelineInfo.renderPass = renderPass;

  vkCreateGraphicsPipelines(vkDev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                            &m_pipeline);

  rasterizer.polygonMode = VK_POLYGON_MODE_LINE;
  pipelineInfo.pRasterizationState = &rasterizer;
  vkCreateGraphicsPipelines(vkDev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                            &m_wireframePipeline);

  vkDestroyShaderModule(vkDev, vertModule, nullptr);
  vkDestroyShaderModule(vkDev, fragModule, nullptr);
  spdlog::info("Terrain pipeline created (fill + wireframe)");
}

// ── Water resources ─────────────────────────────────────────────────────────

void TerrainSystem::createWaterResources(const Device &device,
                                         VkRenderPass renderPass) {
  VkDevice vkDev = device.getDevice();
  VmaAllocator allocator = device.getAllocator();

  // Water quad: full-map plane at Y = 0.8
  struct WaterVertex {
    glm::vec3 pos;
    glm::vec2 uv;
  };
  float y = 0.8f;
  WaterVertex waterVerts[] = {{{0.0f, y, 0.0f}, {0.0f, 0.0f}},
                              {{m_worldSize, y, 0.0f}, {1.0f, 0.0f}},
                              {{m_worldSize, y, m_worldSize}, {1.0f, 1.0f}},
                              {{0.0f, y, m_worldSize}, {0.0f, 1.0f}}};
  uint32_t waterIndices[] = {0, 2, 1, 0, 3, 2};

  // Upload water vertex buffer
  auto uploadBuf = [&](const void *data, VkDeviceSize size,
                       VkBufferUsageFlags usage, VkBuffer &outBuf,
                       void *&outAlloc) {
    VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = size;
    bufInfo.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VmaAllocation alloc;
    vmaCreateBuffer(allocator, &bufInfo, &ai, &outBuf, &alloc, nullptr);
    outAlloc = alloc;

    VkBuffer stg;
    VmaAllocation stgA;
    VkBufferCreateInfo si{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    si.size = size;
    si.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo sai{};
    sai.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    vmaCreateBuffer(allocator, &si, &sai, &stg, &stgA, nullptr);
    void *m;
    vmaMapMemory(allocator, stgA, &m);
    std::memcpy(m, data, size);
    vmaUnmapMemory(allocator, stgA);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo ci{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ci.commandPool = device.getTransferCommandPool();
    ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ci.commandBufferCount = 1;
    vkAllocateCommandBuffers(vkDev, &ci, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkBufferCopy cp{0, 0, size};
    vkCmdCopyBuffer(cmd, stg, outBuf, 1, &cp);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cmd;
    vkQueueSubmit(device.getGraphicsQueue(), 1, &sub, VK_NULL_HANDLE);
    vkQueueWaitIdle(device.getGraphicsQueue());
    vkFreeCommandBuffers(vkDev, device.getTransferCommandPool(), 1, &cmd);
    vmaDestroyBuffer(allocator, stg, stgA);
  };

  uploadBuf(waterVerts, sizeof(waterVerts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            m_waterVbVma, m_waterVbAlloc);
  uploadBuf(waterIndices, sizeof(waterIndices),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_waterIbVma, m_waterIbAlloc);

  // Water pipeline
  std::string shaderDir = SHADER_DIR;
  VkShaderModule wVert =
      createShaderModule(vkDev, shaderDir + "water.vert.spv");
  VkShaderModule wFrag =
      createShaderModule(vkDev, shaderDir + "water.frag.spv");

  VkPipelineShaderStageCreateInfo wStages[2]{};
  wStages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_VERTEX_BIT,
                wVert,
                "main"};
  wStages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                wFrag,
                "main"};

  // Water vertex input: pos (vec3) + uv (vec2)
  VkVertexInputBindingDescription waterBinding{0, sizeof(WaterVertex),
                                               VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription waterAttrs[] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(WaterVertex, pos)},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(WaterVertex, uv)}};

  VkPipelineVertexInputStateCreateInfo waterVertexInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  waterVertexInput.vertexBindingDescriptionCount = 1;
  waterVertexInput.pVertexBindingDescriptions = &waterBinding;
  waterVertexInput.vertexAttributeDescriptionCount = 2;
  waterVertexInput.pVertexAttributeDescriptions = waterAttrs;

  VkPipelineInputAssemblyStateCreateInfo iasm{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  iasm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkDynamicState ds[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dsi{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dsi.dynamicStateCount = 2;
  dsi.pDynamicStates = ds;

  VkPipelineViewportStateCreateInfo vps{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vps.viewportCount = 1;
  vps.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rast{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rast.polygonMode = VK_POLYGON_MODE_FILL;
  rast.lineWidth = 1.0f;
  rast.cullMode = VK_CULL_MODE_NONE; // visible from both sides
  rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_FALSE; // read-only depth for transparency
  depth.depthCompareOp = VK_COMPARE_OP_LESS;

  // Alpha blending
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

  // Water pipeline layout: set 0 = UBO (shared), set 1 = water textures
  VkDescriptorSetLayout waterLayouts[] = {m_uboDescLayout,
                                          m_waterTexDescLayout};
  VkPipelineLayoutCreateInfo wli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  wli.setLayoutCount = 2;
  wli.pSetLayouts = waterLayouts;
  vkCreatePipelineLayout(vkDev, &wli, nullptr, &m_waterPipelineLayout);

  VkGraphicsPipelineCreateInfo wpi{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  wpi.stageCount = 2;
  wpi.pStages = wStages;
  wpi.pVertexInputState = &waterVertexInput;
  wpi.pInputAssemblyState = &iasm;
  wpi.pViewportState = &vps;
  wpi.pRasterizationState = &rast;
  wpi.pMultisampleState = &ms;
  wpi.pDepthStencilState = &depth;
  wpi.pColorBlendState = &cb;
  wpi.pDynamicState = &dsi;
  wpi.layout = m_waterPipelineLayout;
  wpi.renderPass = renderPass;

  vkCreateGraphicsPipelines(vkDev, VK_NULL_HANDLE, 1, &wpi, nullptr,
                            &m_waterPipeline);

  vkDestroyShaderModule(vkDev, wVert, nullptr);
  vkDestroyShaderModule(vkDev, wFrag, nullptr);
  spdlog::info("Water pipeline created (alpha-blended)");
}

// ── Initialization ──────────────────────────────────────────────────────────

void TerrainSystem::init(const Device &device, VkRenderPass renderPass,
                         const std::string &heightmapPath) {
  m_device = &device;

  if (heightmapPath.empty())
    generateProceduralHeightmap();
  else
    loadHeightmap(heightmapPath);

  // Generate terrain textures
  m_textures.generate(device);

  buildMesh();
  createDescriptors(device);
  createPipeline(device, renderPass);
  createWaterResources(device, renderPass);

  m_initialized = true;
  spdlog::info("TerrainSystem initialized ({} chunks, {} indices, 6 textures, "
               "water plane)",
               m_chunks.size(), m_totalIndices);
}

// ── Render ──────────────────────────────────────────────────────────────────

void TerrainSystem::render(VkCommandBuffer cmd, const glm::mat4 &view,
                           const glm::mat4 &proj, const glm::vec3 &cameraPos,
                           const Frustum &frustum, float time, bool wireframe) {
  if (!m_initialized)
    return;

  // Update UBO
  TerrainUBO ubo{};
  ubo.view = view;
  ubo.proj = proj;
  ubo.cameraPos = glm::vec4(cameraPos, 1.0f);
  ubo.time = time;
  std::memcpy(m_uboMapped, &ubo, sizeof(TerrainUBO));

  // ── Terrain pass (opaque) ───────────────────────────────────────────────
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    wireframe ? m_wireframePipeline : m_pipeline);

  VkDescriptorSet terrainSets[] = {m_uboDescSet, m_texDescSet};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipelineLayout, 0, 2, terrainSets, 0, nullptr);

  VkBuffer vbBuffers[] = {m_vbVma};
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(cmd, 0, 1, vbBuffers, offsets);
  vkCmdBindIndexBuffer(cmd, m_ibVma, 0, VK_INDEX_TYPE_UINT32);

  m_visibleChunks = 0;
  for (const auto &chunk : m_chunks) {
    if (!frustum.isVisible(chunk.aabb))
      continue;
    vkCmdDrawIndexed(cmd, chunk.indexCount, 1, chunk.indexOffset, 0, 0);
    ++m_visibleChunks;
  }

  // ── Water pass (transparent, after terrain) ─────────────────────────────
  if (!wireframe) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterPipeline);

    VkDescriptorSet waterSets[] = {m_uboDescSet, m_waterTexDescSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_waterPipelineLayout, 0, 2, waterSets, 0, nullptr);

    VkBuffer waterVb[] = {m_waterVbVma};
    vkCmdBindVertexBuffers(cmd, 0, 1, waterVb, offsets);
    vkCmdBindIndexBuffer(cmd, m_waterIbVma, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
  }
}

// ── Height query ────────────────────────────────────────────────────────────

float TerrainSystem::GetHeightAt(float worldX, float worldZ) const {
  if (m_heightmap.empty())
    return 0.0f;
  float hmX = std::clamp((worldX / m_worldSize) * (m_hmSize - 1), 0.0f,
                         static_cast<float>(m_hmSize - 1));
  float hmZ = std::clamp((worldZ / m_worldSize) * (m_hmSize - 1), 0.0f,
                         static_cast<float>(m_hmSize - 1));
  int x0 = static_cast<int>(std::floor(hmX)),
      z0 = static_cast<int>(std::floor(hmZ));
  int x1 = std::min(x0 + 1, m_hmSize - 1), z1 = std::min(z0 + 1, m_hmSize - 1);
  float fx = hmX - x0, fz = hmZ - z0;
  float h0 =
      m_heightmap[z0 * m_hmSize + x0] +
      (m_heightmap[z0 * m_hmSize + x1] - m_heightmap[z0 * m_hmSize + x0]) * fx;
  float h1 =
      m_heightmap[z1 * m_hmSize + x0] +
      (m_heightmap[z1 * m_hmSize + x1] - m_heightmap[z1 * m_hmSize + x0]) * fx;
  return (h0 + (h1 - h0) * fz) * m_heightScale;
}

} // namespace glory
