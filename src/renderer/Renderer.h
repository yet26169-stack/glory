#pragma once

#include "ability/ProjectileSystem.h"
#include "camera/Camera.h"
#include "combat/AutoAttackSystem.h"
#include "hud/HUD.h"
#include "hud/MinionHealthBars.h"
#include "input/TargetingSystem.h"
#include "editor/DebugOverlay.h"
#include "fog/FogSystem.h"
#include "input/InputManager.h"
#include "map/MapLoader.h"
#include "minion/MinionSystem.h"
#include "structure/StructureSystem.h"
#include "jungle/JungleSystem.h"
#include "nav/DebugRenderer.h"
#include "renderer/Bloom.h"
#include "renderer/Buffer.h"
#include "renderer/ComputeSkinner.h"
#include "renderer/Context.h"
#include "renderer/ClickIndicatorRenderer.h"
#include "renderer/Descriptors.h"
#include "renderer/Device.h"
#include "renderer/Model.h"
#include "renderer/ParticleSystem.h"
#include "renderer/Pipeline.h"
#include "renderer/PostProcess.h"
#include "renderer/SSAO.h"
#include "renderer/ShadowMap.h"
#include "renderer/StaticSkinnedMesh.h"
#include "renderer/GpuProfiler.h"
#include "renderer/GpuCuller.h"
#include "renderer/CascadeShadow.h"
#include "renderer/Swapchain.h"
#include "renderer/Sync.h"
#include "renderer/Texture.h"
#include "renderer/TextureStreamer.h"
#include "scene/Scene.h"
#include "terrain/IsometricCamera.h"
#include "terrain/TerrainSystem.h"

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <tuple>
#include <unordered_map>
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
  std::unique_ptr<GpuProfiler> m_gpuProfiler;
  GpuCuller     m_gpuCuller;                // GPU frustum culling compute pass
  std::unique_ptr<CascadeShadow> m_cascadeShadow;            // 3-cascade shadow maps (MOBA mode)
  ComputeSkinner m_computeSkinner;          // Compute pre-skinning (>50 entities)
  TextureStreamer m_textureStreamer;         // Async texture loading via transfer queue

  std::unique_ptr<DebugOverlay> m_overlay;

  Scene m_scene;

  Camera m_camera;
  std::unique_ptr<InputManager> m_input;

  uint32_t m_currentFrame = 0;
  float m_lastFrameTime   = 0.0f;
  float m_simAccumulator  = 0.0f;   // fixed-timestep accumulator
  bool m_wireframe = false;
  bool m_showGrid  = false;
  bool m_mobaMode  = true; // F4 toggles MOBA terrain view

  // MOBA terrain system
  std::unique_ptr<TerrainSystem> m_terrain;
  IsometricCamera m_isoCam;

  MapData m_mapData;
  DebugRenderer m_debugRenderer;
  void drawMapDebugLines();

  // Minion NPC system
  MinionSystem m_minionSystem;
  float m_gameTime = 0.0f; // total elapsed game time in seconds
  uint32_t m_minionMeshIndices[4] = {};  // per-type meshes (melee/caster/siege/super)
  uint32_t m_minionDefaultTex = 0;   // white 1×1 texture
  uint32_t m_minionFlatNorm = 0;     // flat normal map

  // Skinned minion templates: loaded once at startup, copied to each spawned entity
  struct MinionTemplate {
    Skeleton                              skeleton;
    std::vector<std::vector<Vertex>>      bindPoseVertices;
    std::vector<std::vector<SkinVertex>>  skinVertices;
    std::vector<std::vector<uint32_t>>    indices;
    std::vector<AnimationClip>            animClips;  // [0]=walk, [1]=attack
    uint32_t                              texIndex  = 0;
    uint32_t                              staticSkinnedMeshIndex = UINT32_MAX; // shared GPU mesh
    bool                                  loaded    = false;
  };
  MinionTemplate m_meleeMinionTemplate;
  MinionTemplate m_casterMinionTemplate;

  // Bone slot pooling for GPU-skinned minions (slot 0 reserved for player)
  std::queue<uint32_t>                     m_freeBoneSlots;
  std::unordered_map<uint32_t, uint32_t>   m_entityBoneSlot;   // entt entity id → bone slot
  std::unordered_map<uint32_t, Buffer>     m_minionOutputBuffers; // entity id → compute output buf

  // Targeting & auto-attack
  AutoAttackSystem   m_autoAttackSystem;
  TargetingSystem    m_targetingSystem;
  MinionHealthBars   m_minionHealthBars;

  // Structure system
  StructureSystem m_structureSystem;
  uint32_t m_towerMeshIndex     = 0;
  uint32_t m_towerTopMeshIndex  = 0;
  uint32_t m_inhibitorMeshIndex = 0;
  uint32_t m_nexusMeshIndex     = 0;

  // Jungle system
  JungleSystem m_jungleSystem;
  uint32_t m_monsterSmallMeshIndex = 0;
  uint32_t m_monsterBigMeshIndex   = 0;
  uint32_t m_monsterEpicMeshIndex  = 0;

  // Ability system
  ProjectileSystem m_projectileSystem;
  uint32_t m_sphereMeshIndex = 0; // shared sphere mesh for projectiles

  // HUD
  HUD m_hud;
  std::unique_ptr<ClickIndicatorRenderer> m_clickIndicatorRenderer;
  entt::entity m_playerEntity = entt::null;

  // Click indicator (world-space octagon + arrows animation on right-click)
  struct ClickIndicator {
    glm::vec3 position{0.0f};
    float     lifetime = 0.0f;
    float     maxLife  = 1.2f;  // total animation duration
    bool      isAttack = false; // true = red, false = green
  };
  std::optional<ClickIndicator> m_clickIndicator;
  bool m_glbMapLoaded  = false; // true when GLB map mesh loaded successfully
  bool m_customMap     = false; // true when custom flat map tiles are active
  void buildHeightmapFromGLB();  // rasterize GLB mesh Y onto terrain heightmap

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

  // GPU-skinned character pipeline (SkinnedVertex + bone SSBO at binding 4)
  VkPipeline       m_skinnedPipeline              = VK_NULL_HANDLE;
  VkPipelineLayout m_skinnedPipelineLayout        = VK_NULL_HANDLE;
  // Skinned shadow pipeline (same vertex layout, shadow.frag)
  VkPipeline       m_shadowSkinnedPipeline        = VK_NULL_HANDLE;
  VkPipelineLayout m_shadowSkinnedPipelineLayout  = VK_NULL_HANDLE;

  // Compute-skinned output buffers: one VkBuffer per registered character mesh.
  // Allocated once in buildScene(), indexed by character slot (0..N-1).
  struct ComputeSkinEntry {
      Buffer        outputBuffer;
      uint32_t      vertexCount  = 0;
      uint32_t      entitySlot   = 0; // index into skinned entity list
  };
  std::vector<ComputeSkinEntry> m_computeSkinEntries;
  // Active skin batches assembled each frame (passed to ComputeSkinner::dispatch)
  std::vector<SkinBatch>        m_activeSkinBatches;

  void buildScene();
  void recreateSwapchain();
  void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex,
                           float deltaTime);
  glm::mat4 computeLightVP() const;
  void createSkyPipeline();
  void destroySkyPipeline();
  void createGridPipeline();
  void destroyGridPipeline();
  void createSkinnedPipeline();
  void destroySkinnedPipeline();
};

} // namespace glory
