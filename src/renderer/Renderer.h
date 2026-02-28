#pragma once

#include "ability/ProjectileSystem.h"
#include "camera/Camera.h"
#include "editor/DebugOverlay.h"
#include "fog/FogSystem.h"
#include "input/InputManager.h"
#include "map/MapLoader.h"
#include "nav/DebugRenderer.h"
#include "renderer/Bloom.h"
#include "renderer/Buffer.h"
#include "renderer/Context.h"
#include "renderer/Descriptors.h"
#include "renderer/Device.h"
#include "renderer/Model.h"
#include "renderer/ParticleSystem.h"
#include "renderer/Pipeline.h"
#include "renderer/PostProcess.h"
#include "renderer/SSAO.h"
#include "renderer/ShadowMap.h"
#include "renderer/Swapchain.h"
#include "renderer/Sync.h"
#include "renderer/Texture.h"
#include "scene/Scene.h"
#include "terrain/IsometricCamera.h"
#include "terrain/TerrainSystem.h"

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace glory {

class Window;

class Renderer {
public:
  explicit Renderer(Window &window);
  ~Renderer();

  Renderer(const Renderer &) = delete;
  Renderer &operator=(const Renderer &) = delete;

  void drawFrame();
  void waitIdle();

private:
  Window &m_window;

  std::unique_ptr<Context> m_context;
  std::unique_ptr<Device> m_device;
  std::unique_ptr<Swapchain> m_swapchain;
  std::unique_ptr<ShadowMap> m_shadowMap;
  std::unique_ptr<PostProcess> m_postProcess;
  std::unique_ptr<Bloom> m_bloom;
  std::unique_ptr<class SSAO> m_ssao;
  std::unique_ptr<ParticleSystem> m_particles;
  std::unique_ptr<Descriptors> m_descriptors;
  std::unique_ptr<Pipeline> m_pipeline;
  std::unique_ptr<Sync> m_sync;

  std::unique_ptr<DebugOverlay> m_overlay;

  Scene m_scene;

  Camera m_camera;
  std::unique_ptr<InputManager> m_input;

  uint32_t m_currentFrame = 0;
  float m_lastFrameTime = 0.0f;
  bool m_wireframe = false;
  bool m_showGrid = false;
  bool m_mobaMode = true; // F4 toggles MOBA terrain view

  // MOBA terrain system
  std::unique_ptr<TerrainSystem> m_terrain;
  IsometricCamera m_isoCam;

  MapData m_mapData;
  DebugRenderer m_debugRenderer;
  void drawMapDebugLines();

  // Ability system
  ProjectileSystem m_projectileSystem;
  uint32_t m_sphereMeshIndex = 0; // shared sphere mesh for projectiles

  // Per-frame instance buffers (CPU_TO_GPU, persistently mapped)
  static constexpr uint32_t INITIAL_INSTANCE_CAPACITY = 1024;
  std::vector<Buffer> m_instanceBuffers;
  std::vector<void *> m_instanceMapped;
  uint32_t m_instanceCapacity = 0;

  // Per-frame indirect draw buffers (CPU_TO_GPU, persistently mapped)
  std::vector<Buffer> m_indirectBuffers;
  std::vector<void *> m_indirectMapped;
  uint32_t m_indirectCapacity = 0;

  // Draw group for instanced + indirect rendering
  struct DrawGroup {
    uint32_t meshIndex;
    uint32_t textureIndex;
    uint32_t normalMapIndex;
    uint32_t instanceOffset;
    uint32_t instanceCount;
    uint32_t indirectOffset; // index of first command in indirect buffer
    uint32_t
        indirectCount; // number of indirect commands (= mesh count in model)
  };

  void createInstanceBuffers();
  void destroyInstanceBuffers();
  void createIndirectBuffers();
  void destroyIndirectBuffers();

  // Per-thread command pools for multithreaded recording
  uint32_t m_workerCount = 0;
  std::vector<VkCommandPool> m_threadCommandPools;
  std::vector<std::vector<VkCommandBuffer>>
      m_secondaryBuffers; // [frame][worker]
  void createThreadResources();
  void destroyThreadResources();

  // Sky gradient pipeline (fullscreen triangle, no depth, drawn before scene)
  VkPipelineLayout m_skyPipelineLayout = VK_NULL_HANDLE;
  VkPipeline m_skyPipeline = VK_NULL_HANDLE;

  // Debug grid pipeline (alpha-blended quad at floor level)
  VkPipelineLayout m_gridPipelineLayout = VK_NULL_HANDLE;
  VkPipeline m_gridPipeline = VK_NULL_HANDLE;

  void buildScene();
  void recreateSwapchain();
  void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex,
                           float deltaTime);
  glm::mat4 computeLightVP() const;
  void createSkyPipeline();
  void destroySkyPipeline();
  void createGridPipeline();
  void destroyGridPipeline();
};

} // namespace glory
