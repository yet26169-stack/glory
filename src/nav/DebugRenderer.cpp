#include "nav/DebugRenderer.h"
#include "renderer/Device.h"

#include <spdlog/spdlog.h>
#include <vk_mem_alloc.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace glory {

DebugRenderer::~DebugRenderer() { cleanup(); }

void DebugRenderer::cleanup() {
  if (!m_initialized || !m_device)
    return;
  VkDevice vkDev = m_device->getDevice();
  VmaAllocator allocator = m_device->getAllocator();
  vkDeviceWaitIdle(vkDev);

  if (m_pipeline)
    vkDestroyPipeline(vkDev, m_pipeline, nullptr);
  if (m_pipelineLayout)
    vkDestroyPipelineLayout(vkDev, m_pipelineLayout, nullptr);
  if (m_vertexBuffer)
    vmaDestroyBuffer(allocator, m_vertexBuffer,
                     static_cast<VmaAllocation>(m_vertexAlloc));

  m_pipeline = VK_NULL_HANDLE;
  m_pipelineLayout = VK_NULL_HANDLE;
  m_vertexBuffer = VK_NULL_HANDLE;
  m_vertexAlloc = nullptr;
  m_initialized = false;
}

// ── Draw commands ───────────────────────────────────────────────────────────

void DebugRenderer::drawLine(const glm::vec3 &a, const glm::vec3 &b,
                             const glm::vec4 &color) {
  m_vertices.push_back({a, color});
  m_vertices.push_back({b, color});
}

void DebugRenderer::drawCircle(const glm::vec3 &center, float radius,
                               const glm::vec4 &color, int segments) {
  for (int i = 0; i < segments; ++i) {
    float a0 =
        static_cast<float>(i) / segments * 2.0f * static_cast<float>(M_PI);
    float a1 =
        static_cast<float>(i + 1) / segments * 2.0f * static_cast<float>(M_PI);
    glm::vec3 p0 =
        center + glm::vec3(std::cos(a0) * radius, 0.0f, std::sin(a0) * radius);
    glm::vec3 p1 =
        center + glm::vec3(std::cos(a1) * radius, 0.0f, std::sin(a1) * radius);
    drawLine(p0, p1, color);
  }
}

void DebugRenderer::drawAABB(const glm::vec3 &min, const glm::vec3 &max,
                             const glm::vec4 &color) {
  // Bottom face
  drawLine({min.x, min.y, min.z}, {max.x, min.y, min.z}, color);
  drawLine({max.x, min.y, min.z}, {max.x, min.y, max.z}, color);
  drawLine({max.x, min.y, max.z}, {min.x, min.y, max.z}, color);
  drawLine({min.x, min.y, max.z}, {min.x, min.y, min.z}, color);
  // Top face
  drawLine({min.x, max.y, min.z}, {max.x, max.y, min.z}, color);
  drawLine({max.x, max.y, min.z}, {max.x, max.y, max.z}, color);
  drawLine({max.x, max.y, max.z}, {min.x, max.y, max.z}, color);
  drawLine({min.x, max.y, max.z}, {min.x, max.y, min.z}, color);
  // Verticals
  drawLine({min.x, min.y, min.z}, {min.x, max.y, min.z}, color);
  drawLine({max.x, min.y, min.z}, {max.x, max.y, min.z}, color);
  drawLine({max.x, min.y, max.z}, {max.x, max.y, max.z}, color);
  drawLine({min.x, min.y, max.z}, {min.x, max.y, max.z}, color);
}

void DebugRenderer::drawSphere(const glm::vec3 &center, float radius,
                               const glm::vec4 &color, int segments) {
  // Three orthogonal circles
  drawCircle(center, radius, color, segments); // XZ plane
  // XY plane
  for (int i = 0; i < segments; ++i) {
    float a0 =
        static_cast<float>(i) / segments * 2.0f * static_cast<float>(M_PI);
    float a1 =
        static_cast<float>(i + 1) / segments * 2.0f * static_cast<float>(M_PI);
    glm::vec3 p0 =
        center + glm::vec3(std::cos(a0) * radius, std::sin(a0) * radius, 0.0f);
    glm::vec3 p1 =
        center + glm::vec3(std::cos(a1) * radius, std::sin(a1) * radius, 0.0f);
    drawLine(p0, p1, color);
  }
  // YZ plane
  for (int i = 0; i < segments; ++i) {
    float a0 =
        static_cast<float>(i) / segments * 2.0f * static_cast<float>(M_PI);
    float a1 =
        static_cast<float>(i + 1) / segments * 2.0f * static_cast<float>(M_PI);
    glm::vec3 p0 =
        center + glm::vec3(0.0f, std::cos(a0) * radius, std::sin(a0) * radius);
    glm::vec3 p1 =
        center + glm::vec3(0.0f, std::cos(a1) * radius, std::sin(a1) * radius);
    drawLine(p0, p1, color);
  }
}

void DebugRenderer::clear() { m_vertices.clear(); }

// ── Buffer management ───────────────────────────────────────────────────────

void DebugRenderer::ensureCapacity(size_t needed) {
  if (needed <= m_bufferCapacity)
    return;

  VmaAllocator allocator = m_device->getAllocator();
  // Destroy old
  if (m_vertexBuffer) {
    vmaDestroyBuffer(allocator, m_vertexBuffer,
                     static_cast<VmaAllocation>(m_vertexAlloc));
  }

  m_bufferCapacity = std::max(needed, INITIAL_CAPACITY);
  VkDeviceSize size = m_bufferCapacity * sizeof(DebugVertex);

  VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufInfo.size = size;
  bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
  allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

  VmaAllocationInfo mapInfo{};
  VmaAllocation alloc;
  vmaCreateBuffer(allocator, &bufInfo, &allocInfo, &m_vertexBuffer, &alloc,
                  &mapInfo);
  m_vertexAlloc = alloc;
  m_vertexMapped = mapInfo.pMappedData;
}

// ── Render ──────────────────────────────────────────────────────────────────

void DebugRenderer::render(VkCommandBuffer cmd, const glm::mat4 &viewProj) {
  if (!m_enabled || !m_initialized || m_vertices.empty())
    return;

  ensureCapacity(m_vertices.size());

  // Upload vertex data
  std::memcpy(m_vertexMapped, m_vertices.data(),
              m_vertices.size() * sizeof(DebugVertex));

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

  // Push viewProj as push constant
  vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                     sizeof(glm::mat4), &viewProj);

  VkBuffer buffers[] = {m_vertexBuffer};
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
  vkCmdDraw(cmd, static_cast<uint32_t>(m_vertices.size()), 1, 0, 0);
}

// ── Pipeline creation ───────────────────────────────────────────────────────

std::vector<char> DebugRenderer::readFile(const std::string &filepath) {
  std::ifstream file(filepath, std::ios::ate | std::ios::binary);
  if (!file.is_open())
    throw std::runtime_error("DebugRenderer: cannot open shader: " + filepath);
  size_t fileSize = static_cast<size_t>(file.tellg());
  std::vector<char> buffer(fileSize);
  file.seekg(0);
  file.read(buffer.data(), fileSize);
  return buffer;
}

VkShaderModule
DebugRenderer::createShaderModule(VkDevice device,
                                  const std::string &filepath) const {
  auto code = readFile(filepath);
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = code.size();
  info.pCode = reinterpret_cast<const uint32_t *>(code.data());
  VkShaderModule module;
  if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
    throw std::runtime_error("DebugRenderer: failed to create shader module");
  return module;
}

void DebugRenderer::createPipeline(const Device &device,
                                   VkRenderPass renderPass) {
  VkDevice vkDev = device.getDevice();
  std::string shaderDir = SHADER_DIR;

  VkShaderModule vertModule =
      createShaderModule(vkDev, shaderDir + "debug.vert.spv");
  VkShaderModule fragModule =
      createShaderModule(vkDev, shaderDir + "debug.frag.spv");

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

  // Vertex input: pos (vec3) + color (vec4)
  VkVertexInputBindingDescription binding{0, sizeof(DebugVertex),
                                          VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attrs[] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(DebugVertex, pos)},
      {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DebugVertex, color)}};
  VkPipelineVertexInputStateCreateInfo vertexInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertexInput.vertexBindingDescriptionCount = 1;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = 2;
  vertexInput.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

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
  depth.depthTestEnable = VK_TRUE;
  depth.depthWriteEnable = VK_FALSE; // read-only depth
  depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  // Alpha blending for semi-transparent debug shapes
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

  // Push constant for viewProj matrix
  VkPushConstantRange pushConst{};
  pushConst.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  pushConst.offset = 0;
  pushConst.size = sizeof(glm::mat4);

  VkPipelineLayoutCreateInfo layoutInfo{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &pushConst;
  vkCreatePipelineLayout(vkDev, &layoutInfo, nullptr, &m_pipelineLayout);

  VkGraphicsPipelineCreateInfo pipelineInfo{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &vpState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &ms;
  pipelineInfo.pDepthStencilState = &depth;
  pipelineInfo.pColorBlendState = &cb;
  pipelineInfo.pDynamicState = &dynState;
  pipelineInfo.layout = m_pipelineLayout;
  pipelineInfo.renderPass = renderPass;

  vkCreateGraphicsPipelines(vkDev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                            &m_pipeline);

  vkDestroyShaderModule(vkDev, vertModule, nullptr);
  vkDestroyShaderModule(vkDev, fragModule, nullptr);
  spdlog::info("Debug renderer pipeline created");
}

void DebugRenderer::init(const Device &device, VkRenderPass renderPass) {
  m_device = &device;
  ensureCapacity(INITIAL_CAPACITY);
  createPipeline(device, renderPass);
  m_initialized = true;
  spdlog::info("DebugRenderer initialized");
}

} // namespace glory
