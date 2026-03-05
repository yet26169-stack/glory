#pragma once

#include "renderer/Frustum.h"
#include "renderer/Buffer.h"
#include "terrain/TerrainTextures.h"
#include "terrain/TerrainVertex.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace glory {

class Device;

struct TerrainChunk {
  uint32_t indexCount = 0;
  uint32_t indexOffset = 0;
  uint32_t vertexOffset = 0;
  AABB aabb;
};

struct TerrainUBO {
  glm::mat4 view;
  glm::mat4 proj;
  glm::vec4 cameraPos;
  float time;
  float _pad[3]; // align to 16 bytes
};

class TerrainSystem {
public:
  TerrainSystem() = default;
  ~TerrainSystem();

  TerrainSystem(const TerrainSystem &) = delete;
  TerrainSystem &operator=(const TerrainSystem &) = delete;

  void init(const Device &device, VkRenderPass renderPass,
            const std::string &heightmapPath = "");

  void render(VkCommandBuffer cmd, const glm::mat4 &view, const glm::mat4 &proj,
              const glm::vec3 &cameraPos, const Frustum &frustum, float time,
              bool wireframe = false);

  float GetHeightAt(float worldX, float worldZ) const;

  void cleanup();

  uint32_t getVisibleChunkCount() const { return m_visibleChunks; }
  uint32_t getTotalChunkCount() const {
    return static_cast<uint32_t>(m_chunks.size());
  }
  int getHeightmapSize() const { return m_hmSize; }
  float getWorldSize() const { return m_worldSize; }
  float getHeightScale() const { return m_heightScale; }
  const std::vector<TerrainChunk> &getChunks() const { return m_chunks; }

  void setHeightmap(const std::vector<float>& hm) {
    if (static_cast<int>(hm.size()) == m_hmSize * m_hmSize)
      m_heightmap = hm;
  }

private:
  const Device *m_device = nullptr;

  // Heightmap
  std::vector<float> m_heightmap;
  int m_hmSize = 256;
  float m_worldSize = 200.0f;
  float m_heightScale = 15.0f;

  // Terrain textures (Phase 3)
  TerrainTextures m_textures;

  // Managed GPU buffers
  Buffer m_vb;
  Buffer m_ib;
  Buffer m_ub;

  // Terrain pipeline
  VkPipeline m_pipeline = VK_NULL_HANDLE;
  VkPipeline m_wireframePipeline = VK_NULL_HANDLE;
  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

  // Descriptor set 0 — UBO
  VkDescriptorSetLayout m_uboDescLayout = VK_NULL_HANDLE;
  VkDescriptorSet m_uboDescSet = VK_NULL_HANDLE;

  // Descriptor set 1 — terrain textures (6 samplers)
  VkDescriptorSetLayout m_texDescLayout = VK_NULL_HANDLE;
  VkDescriptorSet m_texDescSet = VK_NULL_HANDLE;

  // Shared descriptor pool
  VkDescriptorPool m_descPool = VK_NULL_HANDLE;

  // Water pipeline + resources
  VkPipeline m_waterPipeline = VK_NULL_HANDLE;
  VkPipelineLayout m_waterPipelineLayout = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_waterTexDescLayout = VK_NULL_HANDLE;
  VkDescriptorSet m_waterTexDescSet = VK_NULL_HANDLE;
  Buffer m_waterVb;
  Buffer m_waterIb;

  // Chunks
  std::vector<TerrainChunk> m_chunks;
  uint32_t m_totalIndices = 0;
  uint32_t m_totalVertices = 0;
  uint32_t m_visibleChunks = 0;

  bool m_initialized = false;
  int m_chunksPerSide = 8;

  void generateProceduralHeightmap();
  void loadHeightmap(const std::string &path);
  void buildMesh();
  void createPipeline(const Device &device, VkRenderPass renderPass);
  void createDescriptors(const Device &device);
  void createWaterResources(const Device &device, VkRenderPass renderPass);

  VkShaderModule createShaderModule(VkDevice device,
                                    const std::string &filepath) const;
  static std::vector<char> readFile(const std::string &filepath);
};

} // namespace glory
