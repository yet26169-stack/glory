#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace glory {

class Device;

/// Entity contributing to vision.
struct VisionEntity {
  glm::vec3 position{0.0f};
  float sightRange = 20.0f; // world units
};

/// Fog UBO for the post-process shader.
struct FogUBO {
  glm::mat4 invViewProj;
  glm::vec4 fogColorExplored; // (0,0,0, 0.5)
  glm::vec4 fogColorUnknown;  // (0,0,0, 0.95)
};

/// CPU-side fog of war system.
/// Maintains 128×128 vision + exploration maps and uploads to GPU.
class FogSystem {
public:
  static constexpr int MAP_SIZE = 128;
  static constexpr float WORLD_SIZE = 200.0f;

  FogSystem() = default;
  ~FogSystem();

  FogSystem(const FogSystem &) = delete;
  FogSystem &operator=(const FogSystem &) = delete;

  /// Initialize Vulkan resources (textures, pipeline, descriptors).
  void init(const Device &device, VkRenderPass renderPass,
            VkImageView sceneColorView, VkSampler sceneColorSampler,
            VkImageView sceneDepthView, VkSampler sceneDepthSampler);

  /// CPU-side: paint vision circles, update exploration, upload textures.
  void update(const std::vector<VisionEntity> &entities);

  /// GPU-side: render full-screen fog post-process.
  void render(VkCommandBuffer cmd, const glm::mat4 &invViewProj);

  /// CPU query: is a world position currently visible?
  bool isPositionVisible(float worldX, float worldZ) const;

  /// CPU query: has a position ever been explored?
  bool isPositionExplored(float worldX, float worldZ) const;

  void cleanup();

  // Expose for testing
  const std::vector<uint8_t> &getVisionBuffer() const { return m_visionBuffer; }
  const std::vector<uint8_t> &getExplorationBuffer() const {
    return m_explorationBuffer;
  }

  // Allow direct buffer manipulation for tests (no Vulkan)
  void updateCpuOnly(const std::vector<VisionEntity> &entities);

private:
  const Device *m_device = nullptr;
  bool m_initialized = false;

  // CPU buffers
  std::vector<uint8_t> m_visionBuffer;
  std::vector<uint8_t> m_explorationBuffer;

  // GPU textures (R8 UNORM)
  VkImage m_visionImage = VK_NULL_HANDLE;
  VkImageView m_visionView = VK_NULL_HANDLE;
  VkSampler m_visionSampler = VK_NULL_HANDLE;
  void *m_visionAlloc = nullptr;

  VkImage m_explorationImage = VK_NULL_HANDLE;
  VkImageView m_explorationView = VK_NULL_HANDLE;
  VkSampler m_explorationSampler = VK_NULL_HANDLE;
  void *m_explorationAlloc = nullptr;

  // Staging buffer
  VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
  void *m_stagingAlloc = nullptr;
  void *m_stagingMapped = nullptr;

  // UBO
  VkBuffer m_uboBuffer = VK_NULL_HANDLE;
  void *m_uboAlloc = nullptr;
  void *m_uboMapped = nullptr;

  // Pipeline
  VkPipeline m_pipeline = VK_NULL_HANDLE;
  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_descPool = VK_NULL_HANDLE;
  VkDescriptorSet m_descSet = VK_NULL_HANDLE;

  void paintVisionCircle(float worldX, float worldZ, float radius);
  void uploadTextures();
  void createTextures(const Device &device);
  void createPipeline(const Device &device, VkRenderPass renderPass);
  void createDescriptors(const Device &device, VkImageView sceneColorView,
                         VkSampler sceneColorSampler,
                         VkImageView sceneDepthView,
                         VkSampler sceneDepthSampler);

  VkShaderModule createShaderModule(VkDevice device,
                                    const std::string &filepath) const;
  static std::vector<char> readFile(const std::string &filepath);
};

} // namespace glory
