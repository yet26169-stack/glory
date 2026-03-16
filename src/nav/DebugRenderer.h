#pragma once

#include "renderer/Buffer.h"
#include "renderer/RenderFormats.h"
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vector>

namespace glory {

class Device;
class FlowField;

struct DebugVertex {
  glm::vec3 pos;
  glm::vec4 color;
};

/// Immediate-mode debug line renderer.
class DebugRenderer {
public:
  DebugRenderer() = default;
  ~DebugRenderer();

  void init(const Device &device, const RenderFormats &formats);
  void cleanup();

  // ── Draw commands (call during update) ──────────────────────────────
  void drawLine(const glm::vec3 &a, const glm::vec3 &b, const glm::vec4 &color);
  void drawCircle(const glm::vec3 &center, float radius, const glm::vec4 &color,
                  int segments = 32);
  void drawAABB(const glm::vec3 &min, const glm::vec3 &max,
                const glm::vec4 &color);
  void drawSphere(const glm::vec3 &center, float radius, const glm::vec4 &color,
                  int segments = 8);

  /// Draw flow field direction arrows.  subsample=2 draws every 2nd cell, etc.
  void drawFlowField(const FlowField &field, const glm::vec4 &color,
                     int subsample = 2, float arrowLen = 0.6f, float y = 0.1f);

  // ── Render (call during command buffer recording) ───────────────────
  void render(VkCommandBuffer cmd, const glm::mat4 &viewProj);

  /// Clear vertex buffer at start of each frame.
  void clear();

  void setEnabled(bool enabled) { m_enabled = enabled; }
  bool isEnabled() const { return m_enabled; }
  uint32_t getVertexCount() const {
    return static_cast<uint32_t>(m_vertices.size());
  }

private:
  const Device *m_device = nullptr;
  bool m_enabled = true;
  bool m_initialized = false;

  std::vector<DebugVertex> m_vertices;

  // Managed GPU buffer (CPU_TO_GPU, dynamic)
  Buffer m_vertexBuffer;
  size_t m_bufferCapacity = 0;
  static constexpr size_t INITIAL_CAPACITY = 65536;

  // Pipeline
  VkPipeline m_pipeline = VK_NULL_HANDLE;
  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

  void ensureCapacity(size_t needed);
  void createPipeline(const Device &device, const RenderFormats &formats);

  VkShaderModule createShaderModule(VkDevice device,
                                    const std::string &filepath) const;
  static std::vector<char> readFile(const std::string &filepath);
};

} // namespace glory
