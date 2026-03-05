#pragma once

#include "animation/AnimationClip.h"
#include "animation/Skeleton.h"
#include "renderer/Mesh.h"
#include "renderer/Texture.h"

#include <limits>
#include <string>
#include <vector>

namespace glory {

class Device;

class Model {
public:
  Model() = default;

  // Binary mesh format (.gmesh) header
  struct GMeshHeader {
    char magic[4] = {'G', 'M', 'S', 'H'};
    uint32_t version = 1;
    uint32_t meshCount = 0;
  };
  struct GMeshEntry {
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
  };

  static Model loadFromOBJ(const Device &device, VmaAllocator allocator,
                           const std::string &filepath);
  static Model loadFromGMesh(const Device &device, VmaAllocator allocator,
                             const std::string &filepath);

  // glTF Binary (.glb) loader – extracts meshes with positions, normals, UVs
  static Model loadFromGLB(const Device &device, VmaAllocator allocator,
                           const std::string &filepath);

  struct GLBTexture {
    int materialIndex; // glTF material index
    Texture texture;
  };
  // Extract embedded base-color textures from a GLB file
  static std::vector<GLBTexture> loadGLBTextures(const Device &device,
                                              const std::string &filepath);

  // Axis-aligned bounding box
  struct AABB {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};
  };
  // Compute overall AABB across all meshes in a GLB file
  static AABB getGLBBounds(const std::string &filepath);

  // Extract raw vertex positions + indices from a GLB (CPU-side, no GPU)
  struct RawMeshData {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t>  indices;
  };
  static RawMeshData getGLBRawMesh(const std::string &filepath);

  // Cook OBJ to binary gmesh (build-time, no GPU needed)
  static bool cookOBJ(const std::string &objPath, const std::string &gmeshPath);

  static Model createTriangle(const Device &device, VmaAllocator allocator);
  static Model createCube(const Device &device, VmaAllocator allocator);
  static Model createSphere(const Device &device, VmaAllocator allocator,
                            uint32_t stacks = 32, uint32_t slices = 64);
  static Model createTorus(const Device &device, VmaAllocator allocator,
                           float majorR = 0.7f, float minorR = 0.25f,
                           uint32_t rings = 48, uint32_t sides = 24);
  static Model createCone(const Device &device, VmaAllocator allocator,
                          float radius = 0.5f, float height = 1.0f,
                          uint32_t slices = 32);
  static Model createCylinder(const Device &device, VmaAllocator allocator,
                              float radius = 0.5f, float height = 1.0f,
                              uint32_t slices = 32);
  static Model createCapsule(const Device &device, VmaAllocator allocator,
                             float radius = 0.3f, float height = 0.8f,
                             uint32_t stacks = 8, uint32_t slices = 24);
  static Model createTerrain(const Device &device, VmaAllocator allocator,
                             float size = 10.0f, uint32_t resolution = 64,
                             float heightScale = 1.5f);
  static Model createTorusKnot(const Device &device, VmaAllocator allocator,
                               uint32_t p = 2, uint32_t q = 3,
                               float radius = 0.6f, float tubeRadius = 0.15f,
                               uint32_t segments = 128, uint32_t sides = 16);
  static Model createIcosphere(const Device &device, VmaAllocator allocator,
                               uint32_t subdivisions = 2, float radius = 1.0f);
  static Model createSpring(const Device &device, VmaAllocator allocator,
                            float coilRadius = 0.5f, float tubeRadius = 0.08f,
                            float height = 1.5f, uint32_t coils = 5,
                            uint32_t segments = 128, uint32_t sides = 12);
  static Model createGear(const Device &device, VmaAllocator allocator,
                          float outerRadius = 1.0f, float innerRadius = 0.7f,
                          float hubRadius = 0.3f, float thickness = 0.2f,
                          uint32_t teeth = 16);
  static Model createPyramid(const Device &device, VmaAllocator allocator,
                              float baseSize = 1.0f, float height = 1.5f);

  // Load a skinned GLB: mesh + skeleton + animations + per-vertex bone data
  // (declared here but defined after SkinnedModelData below)
  static struct SkinnedModelData
  loadSkinnedFromGLB(const Device &device, VmaAllocator allocator,
                     const std::string &filepath,
                     float targetReduction = 0.0f); // 0 = no simplification

  void draw(VkCommandBuffer cmd) const;
  void drawInstanced(VkCommandBuffer cmd, uint32_t instanceCount,
                     uint32_t firstInstance) const;
  void drawIndirect(VkCommandBuffer cmd, VkBuffer indirectBuffer,
                    VkDeviceSize offset) const;
  void drawMeshIndirect(VkCommandBuffer cmd, uint32_t meshIdx, VkBuffer indirectBuffer,
                        VkDeviceSize offset) const;

  uint32_t getIndexCount() const;
  uint32_t getMeshCount() const;
  uint32_t getMeshIndexCount(uint32_t meshIdx) const;
  int getMeshMaterialIndex(uint32_t meshIdx) const;

  Model(const Model &) = delete;
  Model &operator=(const Model &) = delete;
  Model(Model &&) noexcept = default;
  Model &operator=(Model &&) noexcept = default;

private:
  std::vector<Mesh> m_meshes;
  std::vector<int> m_meshMaterialIndices; // glTF material index per sub-mesh
};

// Data extracted from a skinned GLB file (skeleton, animations, per-vertex
// bone influences, and a CPU-side copy of bind-pose vertices for skinning).
struct SkinnedModelData {
  Model model;
  Skeleton skeleton;
  std::vector<AnimationClip> animations;
  std::vector<std::vector<SkinVertex>> skinVertices; // per-mesh
  std::vector<std::vector<Vertex>> bindPoseVertices; // per-mesh (CPU copy)
  std::vector<std::vector<uint32_t>> indices;        // per-mesh
};

} // namespace glory
