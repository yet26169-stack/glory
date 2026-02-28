#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <vector>

namespace glory {

class Device;

struct DebugVertex {
  glm::vec3 pos;
  glm::vec4 color;
};

/// Immediate-mode debug line renderer.
/// Buffer draw commands each frame, then flush in a single draw call.
class DebugRenderer {
public:
  DebugRenderer() = default;
  ~DebugRenderer();

  void init(const Device &device, VkRenderPass renderPass);
  void cleanup();

  // ── Draw commands (call during update) ──────────────────────────────
  void drawLine(const glm::vec3 &a, const glm::vec3 &b, const glm::vec4 &color);
  void drawCircle(const glm::vec3 &center, float radius, const glm::vec4 &color,
                  int segments = 32);
  void drawAABB(const glm::vec3 &min, const glm::vec3 &max,
                const glm::vec4 &color);
  void drawSphere(const glm::vec3 &center, float radius, const glm::vec4 &color,
                  int segments = 8);

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

  // VMA buffer (CPU_TO_GPU, dynamic)
  VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
  void *m_vertexAlloc = nullptr;
  void *m_vertexMapped = nullptr;
  size_t m_bufferCapacity = 0;
  static constexpr size_t INITIAL_CAPACITY = 65536;

  // Pipeline
  VkPipeline m_pipeline = VK_NULL_HANDLE;
  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_descPool = VK_NULL_HANDLE;
  VkDescriptorSet m_descSet = VK_NULL_HANDLE;

  // Push constant for viewProj
  // (simpler than UBO for debug renderer)

  void ensureCapacity(size_t needed);
  void createPipeline(const Device &device, VkRenderPass renderPass);

  VkShaderModule createShaderModule(VkDevice device,
                                    const std::string &filepath) const;
  static std::vector<char> readFile(const std::string &filepath);
};

} // namespace glory
