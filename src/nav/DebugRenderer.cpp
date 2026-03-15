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
  if (!m_initialized || !m_device) return;
  VkDevice dev = m_device->getDevice();
  vkDeviceWaitIdle(dev);

  m_vertexBuffer.destroy();

  if (m_pipeline) vkDestroyPipeline(dev, m_pipeline, nullptr);
  if (m_pipelineLayout) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
  m_initialized = false;
}

void DebugRenderer::drawLine(const glm::vec3 &a, const glm::vec3 &b, const glm::vec4 &color) {
  if (!m_enabled) return;
  m_vertices.push_back({a, color});
  m_vertices.push_back({b, color});
}

void DebugRenderer::drawCircle(const glm::vec3 &center, float radius, const glm::vec4 &color, int segments) {
  if (!m_enabled) return;
  for (int i = 0; i < segments; ++i) {
    float theta = 2.0f * (float)M_PI * (float)i / segments;
    float nextTheta = 2.0f * (float)M_PI * (float)(i + 1) / segments;
    glm::vec3 p1 = center + glm::vec3(radius * std::cos(theta), 0.05f, radius * std::sin(theta));
    glm::vec3 p2 = center + glm::vec3(radius * std::cos(nextTheta), 0.05f, radius * std::sin(nextTheta));
    drawLine(p1, p2, color);
  }
}

void DebugRenderer::drawAABB(const glm::vec3 &min, const glm::vec3 &max, const glm::vec4 &color) {
  if (!m_enabled) return;
  glm::vec3 corners[8] = {
    {min.x, min.y, min.z}, {max.x, min.y, min.z},
    {max.x, max.y, min.z}, {min.x, max.y, min.z},
    {min.x, min.y, max.z}, {max.x, min.y, max.z},
    {max.x, max.y, max.z}, {min.x, max.y, max.z}
  };
  for (int i = 0; i < 4; ++i) {
    drawLine(corners[i], corners[(i + 1) % 4], color);
    drawLine(corners[i + 4], corners[4 + (i + 1) % 4], color);
    drawLine(corners[i], corners[i + 4], color);
  }
}

void DebugRenderer::drawSphere(const glm::vec3 &center, float radius, const glm::vec4 &color, int segments) {
  if (!m_enabled) return;
  for (int i = 0; i < segments; ++i) {
    float lat0 = (float)M_PI * (-0.5f + (float)i / segments);
    float z0 = radius * std::sin(lat0);
    float r0 = radius * std::cos(lat0);
    float lat1 = (float)M_PI * (-0.5f + (float)(i + 1) / segments);
    float z1 = radius * std::sin(lat1);
    float r1 = radius * std::cos(lat1);
    for (int j = 0; j < segments; ++j) {
      float lng = 2.0f * (float)M_PI * (float)j / segments;
      float nextLng = 2.0f * (float)M_PI * (float)(j + 1) / segments;
      float x0 = std::cos(lng), y0 = std::sin(lng);
      float x1 = std::cos(nextLng), y1 = std::sin(nextLng);
      drawLine(center + glm::vec3(r0 * x0, z0, r0 * y0), center + glm::vec3(r0 * x1, z0, r0 * y1), color);
      drawLine(center + glm::vec3(r0 * x0, z0, r0 * y0), center + glm::vec3(r1 * x0, z1, r1 * y0), color);
    }
  }
}

void DebugRenderer::clear() { m_vertices.clear(); }

void DebugRenderer::render(VkCommandBuffer cmd, const glm::mat4 &viewProj) {
  if (!m_enabled || !m_initialized || m_vertices.empty()) return;
  ensureCapacity(m_vertices.size());
  std::memcpy(m_vertexBuffer.map(), m_vertices.data(), m_vertices.size() * sizeof(DebugVertex));

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
  vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &viewProj);
  VkBuffer vbs[] = {m_vertexBuffer.getBuffer()};
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offsets);
  vkCmdDraw(cmd, static_cast<uint32_t>(m_vertices.size()), 1, 0, 0);
}

void DebugRenderer::ensureCapacity(size_t needed) {
  if (needed <= m_bufferCapacity) return;
  m_vertexBuffer.destroy();
  m_bufferCapacity = std::max(needed, m_bufferCapacity * 2);
  m_vertexBuffer = Buffer(m_device->getAllocator(), m_bufferCapacity * sizeof(DebugVertex),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
}

void DebugRenderer::createPipeline(const Device &device, VkRenderPass renderPass) {
  VkDevice dev = device.getDevice();
  VkPushConstantRange pc{};
  pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  pc.size = sizeof(glm::mat4);
  VkPipelineLayoutCreateInfo layoutCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layoutCI.pushConstantRangeCount = 1;
  layoutCI.pPushConstantRanges = &pc;
  vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_pipelineLayout);

  auto vcode = readFile(std::string(SHADER_DIR) + "debug.vert.spv");
  auto fcode = readFile(std::string(SHADER_DIR) + "debug.frag.spv");
  VkShaderModuleCreateInfo smCI{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smCI.codeSize = vcode.size();
  smCI.pCode = (const uint32_t*)vcode.data();
  VkShaderModule vmod; vkCreateShaderModule(dev, &smCI, nullptr, &vmod);
  smCI.codeSize = fcode.size();
  smCI.pCode = (const uint32_t*)fcode.data();
  VkShaderModule fmod; vkCreateShaderModule(dev, &smCI, nullptr, &fmod);

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vmod, "main"};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fmod, "main"};

  VkVertexInputBindingDescription bind{0, sizeof(DebugVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attrs[2] = {
    {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(DebugVertex, pos)},
    {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(DebugVertex, color)}
  };
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
  vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = attrs;

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f; rs.cullMode = VK_CULL_MODE_NONE;

  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;

  VkPipelineColorBlendAttachmentState cbA{};
  cbA.colorWriteMask = 0xF;
  VkPipelineColorBlendAttachmentState dbgBlends[2] = {cbA, {}};
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 2; cb.pAttachments = dbgBlends;

  VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 2; dy.pDynamicStates = dyns;

  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1; vp.scissorCount = 1;

  VkGraphicsPipelineCreateInfo gpCI{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpCI.stageCount = 2; gpCI.pStages = stages; gpCI.pVertexInputState = &vi;
  gpCI.pInputAssemblyState = &ia; gpCI.pViewportState = &vp; gpCI.pRasterizationState = &rs;
  gpCI.pMultisampleState = &ms; gpCI.pDepthStencilState = &ds; gpCI.pColorBlendState = &cb;
  gpCI.pDynamicState = &dy; gpCI.layout = m_pipelineLayout; gpCI.renderPass = renderPass;
  vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpCI, nullptr, &m_pipeline);

  vkDestroyShaderModule(dev, fmod, nullptr);
  vkDestroyShaderModule(dev, vmod, nullptr);
}

std::vector<char> DebugRenderer::readFile(const std::string &path) {
  std::ifstream f(path, std::ios::ate | std::ios::binary);
  size_t sz = (size_t)f.tellg(); std::vector<char> buf(sz);
  f.seekg(0); f.read(buf.data(), sz); return buf;
}

void DebugRenderer::init(const Device &device, VkRenderPass renderPass) {
  m_device = &device;
  ensureCapacity(INITIAL_CAPACITY);
  createPipeline(device, renderPass);
  m_initialized = true;
  spdlog::info("DebugRenderer initialized");
}

} // namespace glory
