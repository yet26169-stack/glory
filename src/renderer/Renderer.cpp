#include "renderer/Renderer.h"
#include "renderer/Frustum.h"
#include "renderer/VkCheck.h"
#include "window/Window.h"
#include "core/Profiler.h"

#include "ability/AbilityComponents.h"
#include "ability/AbilityDef.h"
#include "ability/AbilitySystem.h"
#include "ability/VFXEventQueue.h"
#include "minion/MinionComponents.h"
#include "minion/MinionConfig.h"
#include "structure/StructureComponents.h"
#include "jungle/JungleComponents.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <thread>
#include <tuple>

namespace glory {

Renderer::Renderer(Window &window) : m_window(window) {
  m_context = std::make_unique<Context>();
  m_window.createSurface(m_context->getInstance());
  m_device =
      std::make_unique<Device>(m_context->getInstance(), m_window.getSurface());
  m_swapchain = std::make_unique<Swapchain>(*m_device, m_window.getSurface(),
                                            m_window.getExtent());
  m_shadowMap = std::make_unique<ShadowMap>(*m_device);
  m_postProcess = std::make_unique<PostProcess>(*m_device, *m_swapchain);

  // In MOBA mode, skip creating SSAO and Bloom — they cost ~60MB of GPU memory
  // and 3+ render passes that aren't needed for top-down MOBA rendering.
  // Instead bind 1×1 dummy images to keep descriptor sets valid.
  if (!m_mobaMode) {
    m_bloom = std::make_unique<Bloom>(*m_device, *m_swapchain,
                                      m_postProcess->getHDRImageView());
    m_ssao  = std::make_unique<SSAO>(*m_device, *m_swapchain,
                                      m_postProcess->getHDRDepthView());
    m_postProcess->updateBloomDescriptor(m_bloom->getOutputImageView());
    m_postProcess->updateSSAODescriptor(m_ssao->getOutputImageView());
  } else {
    // Bind dummy 1×1 images — valid descriptors but ~4 bytes of memory each
    m_postProcess->updateBloomDescriptor(m_postProcess->getDummyBloomImageView());
    m_postProcess->updateSSAODescriptor(m_postProcess->getDummySSAOImageView());
    spdlog::info("MOBA mode: SSAO + Bloom skipped (dummy descriptors bound)");
  }
  m_postProcess->updateDepthDescriptor(m_postProcess->getHDRDepthView());
  m_descriptors =
      std::make_unique<Descriptors>(*m_device, Sync::MAX_FRAMES_IN_FLIGHT);
  // Pipeline uses PostProcess's HDR render pass — no own render
  // pass/framebuffers
  m_pipeline = std::make_unique<Pipeline>(*m_device, *m_swapchain,
                                          m_descriptors->getLayout(),
                                          m_postProcess->getHDRRenderPass());
  m_particles = std::make_unique<ParticleSystem>(
      *m_device, m_postProcess->getHDRRenderPass(), 512);
  m_particles->setEmitter(glm::vec3(2.0f, 1.0f, -2.5f), // near torus
                          glm::vec3(1.0f, 0.6f, 0.2f), 40.0f);
  m_sync = std::make_unique<Sync>(*m_device, m_swapchain->getImageCount());
  m_gpuProfiler = std::make_unique<GpuProfiler>(*m_device, Sync::MAX_FRAMES_IN_FLIGHT);
  m_gpuCuller.init(*m_device, Sync::MAX_FRAMES_IN_FLIGHT, 4096);
  if (m_mobaMode) {
    // CSM active in MOBA mode for high-quality shadows across the full viewport
    m_cascadeShadow = std::make_unique<CascadeShadow>(*m_device);
  }
  // Compute skinner (activated at runtime when skinned entity count > threshold)
  m_computeSkinner.init(*m_device, Sync::MAX_FRAMES_IN_FLIGHT);
  // Async texture streamer — fires callback on main thread each tick()
  m_textureStreamer.init(*m_device, [this](uint32_t slot, VkImageView view, VkSampler sampler) {
      // Bind the newly streamed texture into the bindless descriptor array
      m_descriptors->writeBindlessTexture(slot, view, sampler);
      spdlog::info("TextureStreamer: slot {} updated", slot);
  });
  m_overlay = std::make_unique<DebugOverlay>();
  m_overlay->init(m_window.getHandle(), m_context->getInstance(), *m_device,
                  *m_swapchain, m_postProcess->getRenderPass());

  // Load map data before buildScene() so minion system gets valid map positions
  try {
    m_mapData = MapLoader::LoadFromFile(std::string(MAP_DATA_DIR) +
                                        "map_summonersrift.json");
    spdlog::info("Loaded map data");
  } catch (const std::exception &e) {
    spdlog::warn("Failed to load map data: {}", e.what());
  }

  buildScene();
  createSkyPipeline();
  createGridPipeline();
  createSkinnedPipeline();
  createInstanceBuffers();
  createIndirectBuffers();
  createThreadResources();
  // Terrain system
  m_terrain = std::make_unique<TerrainSystem>();
  m_terrain->init(*m_device, m_postProcess->getHDRRenderPass());
  m_isoCam.setBounds(glm::vec3(0, 0, 0), glm::vec3(200, 0, 200));
  m_scene.setTerrainSystem(m_terrain.get());

  // If GLB map was loaded, build a heightmap from the mesh so GetHeightAt()
  // returns correct Y values matching the GLB surface.
  if (m_glbMapLoaded) {
    buildHeightmapFromGLB();
  }

  // Snap player character to terrain surface so it isn't underground on the
  // first frame (it starts at Y=0 but the GLB map surface is higher).
  if (m_mobaMode && m_terrain && m_playerEntity != entt::null) {
    auto &charT = m_scene.getRegistry().get<TransformComponent>(m_playerEntity);
    charT.position.y = m_terrain->GetHeightAt(charT.position.x, charT.position.z);
    spdlog::info("Character snapped to terrain: Y={:.2f}", charT.position.y);
  }
  m_input = std::make_unique<InputManager>(m_window.getHandle(), m_camera);
  m_input->setCaptureEnabled(!m_mobaMode);

  m_debugRenderer.init(*m_device, m_postProcess->getHDRRenderPass());
  m_clickIndicatorRenderer = std::make_unique<ClickIndicatorRenderer>(*m_device, m_postProcess->getHDRRenderPass());

  m_lastFrameTime = static_cast<float>(glfwGetTime());
  spdlog::info("Renderer initialized");
}

Renderer::~Renderer() {
  // 1. Wait for GPU to finish all work before destroying resources
  if (m_device) {
    vkDeviceWaitIdle(m_device->getDevice());
  }

  // 2. Destroy systems and objects that hold GPU resources
  m_input.reset();
  m_overlay.reset();
  m_debugRenderer.cleanup();
  m_clickIndicatorRenderer.reset();
  m_gpuProfiler.reset();
  m_gpuCuller.destroy();
  m_computeSkinner.destroy();
  m_textureStreamer.destroy();
  m_terrain.reset();
  m_particles.reset();
  m_descriptors.reset();
  m_shadowMap.reset();
  m_cascadeShadow.reset();
  m_bloom.reset();
  m_ssao.reset();
  m_postProcess.reset();
  m_sync.reset();

  // 3. Destroy pipelines and buffers
  destroySkyPipeline();
  destroyGridPipeline();
  destroySkinnedPipeline();
  destroyThreadResources();
  destroyIndirectBuffers();
  destroyInstanceBuffers();

  m_computeSkinEntries.clear();

  m_pipeline.reset();
  
  // 5. Explicitly clear the scene to release any entities holding resources
  m_scene = Scene{};

  // 6. Finally destroy device and context
  if (m_device) {
    VkInstance instance = m_context->getInstance();
    m_swapchain.reset();
    m_device.reset();
    m_window.destroySurface(instance);
  }
  m_context.reset();

  spdlog::info("Renderer destroyed");
}

// ── Build heightmap from GLB map mesh ──────────────────────────────────────
// Reads vertex positions from the GLB, transforms to world space using the
// map entity transform, then rasterizes triangle Y values onto the terrain
// heightmap grid so GetHeightAt() returns correct heights for the GLB surface.
void Renderer::buildHeightmapFromGLB() {
  if (!m_terrain) return;

  // Find the map entity to get its transform
  auto mapView = m_scene.getRegistry()
      .view<TransformComponent, MeshComponent, MapComponent>();
  if (mapView.begin() == mapView.end()) return;

  auto mapEntity = *mapView.begin();
  auto &mapT = mapView.get<TransformComponent>(mapEntity);
  glm::mat4 modelMat = mapT.getModelMatrix();

  // Read raw vertex data from GLB (CPU-side, no GPU)
  std::string mapGlbPath = std::string(MODEL_DIR) + "fantasy+arena+3d+model.glb";
  Model::RawMeshData raw;
  try {
    raw = Model::getGLBRawMesh(mapGlbPath);
  } catch (const std::exception &e) {
    spdlog::error("buildHeightmapFromGLB: {}", e.what());
    return;
  }

  if (raw.positions.empty() || raw.indices.size() < 3) return;

  // Transform positions to world space
  std::vector<glm::vec3> worldPositions(raw.positions.size());
  for (size_t i = 0; i < raw.positions.size(); ++i) {
    glm::vec4 wp = modelMat * glm::vec4(raw.positions[i], 1.0f);
    worldPositions[i] = glm::vec3(wp);
  }
  const auto &allIndices = raw.indices;

  int hmSize = m_terrain->getHeightmapSize();
  float worldSize = m_terrain->getWorldSize();
  float heightScale = m_terrain->getHeightScale();

  // Initialize heightmap to 0 (will store absolute Y, then normalize)
  std::vector<float> hm(hmSize * hmSize, 0.0f);
  std::vector<bool> filled(hmSize * hmSize, false);

  // For each triangle, rasterize onto the heightmap grid
  for (size_t ti = 0; ti + 2 < allIndices.size(); ti += 3) {
    const glm::vec3 &v0 = worldPositions[allIndices[ti + 0]];
    const glm::vec3 &v1 = worldPositions[allIndices[ti + 1]];
    const glm::vec3 &v2 = worldPositions[allIndices[ti + 2]];

    // Bounding box in heightmap coords
    float minX = std::min({v0.x, v1.x, v2.x});
    float maxX = std::max({v0.x, v1.x, v2.x});
    float minZ = std::min({v0.z, v1.z, v2.z});
    float maxZ = std::max({v0.z, v1.z, v2.z});

    int ix0 = std::max(0, static_cast<int>(std::floor(minX / worldSize * (hmSize - 1))));
    int ix1 = std::min(hmSize - 1, static_cast<int>(std::ceil(maxX / worldSize * (hmSize - 1))));
    int iz0 = std::max(0, static_cast<int>(std::floor(minZ / worldSize * (hmSize - 1))));
    int iz1 = std::min(hmSize - 1, static_cast<int>(std::ceil(maxZ / worldSize * (hmSize - 1))));

    for (int iz = iz0; iz <= iz1; ++iz) {
      for (int ix = ix0; ix <= ix1; ++ix) {
        float wx = static_cast<float>(ix) / (hmSize - 1) * worldSize;
        float wz = static_cast<float>(iz) / (hmSize - 1) * worldSize;

        // Barycentric test: point (wx, wz) in triangle (v0, v1, v2) in XZ
        glm::vec2 p(wx, wz);
        glm::vec2 a(v0.x, v0.z), b(v1.x, v1.z), c(v2.x, v2.z);
        float denom = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::abs(denom) < 1e-10f) continue;
        float u = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / denom;
        float v = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / denom;
        float w = 1.0f - u - v;
        if (u < -0.001f || v < -0.001f || w < -0.001f) continue;

        float y = u * v0.y + v * v1.y + w * v2.y;
        int idx = iz * hmSize + ix;
        if (!filled[idx] || y > hm[idx]) {
          hm[idx] = y;
          filled[idx] = true;
        }
      }
    }
  }

  // Fill unfilled cells with nearest filled value (simple flood)
  // Multiple passes for simplicity
  for (int pass = 0; pass < 4; ++pass) {
    for (int iz = 0; iz < hmSize; ++iz) {
      for (int ix = 0; ix < hmSize; ++ix) {
        int idx = iz * hmSize + ix;
        if (filled[idx]) continue;
        // Check 4-connected neighbors
        float sum = 0.0f; int count = 0;
        if (ix > 0 && filled[idx - 1]) { sum += hm[idx - 1]; ++count; }
        if (ix < hmSize - 1 && filled[idx + 1]) { sum += hm[idx + 1]; ++count; }
        if (iz > 0 && filled[idx - hmSize]) { sum += hm[idx - hmSize]; ++count; }
        if (iz < hmSize - 1 && filled[idx + hmSize]) { sum += hm[idx + hmSize]; ++count; }
        if (count > 0) {
          hm[idx] = sum / count;
          filled[idx] = true;
        }
      }
    }
  }

  // Normalize: GetHeightAt returns hm[i] * heightScale, so store Y / heightScale
  for (int i = 0; i < hmSize * hmSize; ++i) {
    hm[i] = std::clamp(hm[i] / heightScale, 0.0f, 1.0f);
  }

  m_terrain->setHeightmap(hm);
  spdlog::info("Built heightmap from GLB mesh ({}x{}, heightScale={})",
               hmSize, hmSize, heightScale);
}

void Renderer::drawMapDebugLines() {
  m_debugRenderer.clear();

  // Draw bases
  for (int i = 0; i < 2; ++i) {
    auto color = i == 0 ? glm::vec4(0.2f, 0.5f, 1.0f, 1.0f)
                        : glm::vec4(1.0f, 0.2f, 0.2f, 1.0f);
    auto &team = m_mapData.teams[i];
    m_debugRenderer.drawSphere(team.base.nexusPosition, 3.0f, color);
    m_debugRenderer.drawCircle(team.base.spawnPlatformCenter,
                               team.base.spawnPlatformRadius, color);

    // Draw towers
    for (const auto &tower : team.towers) {
      glm::vec3 pos = (i == 1 && tower.team2Override) ? *tower.team2Override
                                                      : tower.position;
      m_debugRenderer.drawAABB(pos - glm::vec3(1.0f), pos + glm::vec3(1.0f),
                               color);
      m_debugRenderer.drawCircle(pos, tower.attackRange,
                                 glm::vec4(color.r, color.g, color.b, 0.3f));
    }

    // Draw lanes
    for (const auto &lane : team.lanes) {
      if (lane.waypoints.empty())
        continue;
      for (size_t w = 0; w < lane.waypoints.size() - 1; ++w) {
        glm::vec3 p1 = lane.waypoints[w];
        glm::vec3 p2 = lane.waypoints[w + 1];
        if (i == 1 && m_mapData.mapCenter.x > 0) { // Mirror team 2
          p1 = m_mapData.mapCenter * 2.0f - p1;
          p1.y = lane.waypoints[w].y;
          p2 = m_mapData.mapCenter * 2.0f - p2;
          p2.y = lane.waypoints[w + 1].y;
        }
        m_debugRenderer.drawLine(p1, p2, color);
      }
    }
  }

  // Draw neutral camps
  for (const auto &camp : m_mapData.neutralCamps) {
    m_debugRenderer.drawSphere(camp.position, 1.5f,
                               glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
    m_debugRenderer.drawCircle(camp.position, camp.leashRadius,
                               glm::vec4(1.0f, 1.0f, 0.0f, 0.3f));
  }
}

void Renderer::waitIdle() { vkDeviceWaitIdle(m_device->getDevice()); }

// ── Frame rendering ─────────────────────────────────────────────────────────
void Renderer::drawFrame() {
  VkDevice device = m_device->getDevice();

  // Process VFX events
  auto vfxEvents = VFXEventQueue::get().consumeAll();
  for (const auto &ev : vfxEvents) {
    if (ev.type == VFXEventType::IMPACT) {
      // For now, mapping vfxId to simple colors
      glm::vec3 color(1.0f);
      if (ev.vfxId == "vfx_fireball_explosion")
        color = glm::vec3(1.0f, 0.4f, 0.1f);
      else if (ev.vfxId == "vfx_flame_pillar")
        color = glm::vec3(1.0f, 0.2f, 0.0f);
      else if (ev.vfxId == "vfx_molten_shield_pop")
        color = glm::vec3(0.1f, 0.8f, 0.8f);

      if (m_particles) {
        m_particles->setEmitter(ev.position, color, 200.0f);
      }
    }
  }

  float currentTime = static_cast<float>(glfwGetTime());
  float deltaTime = currentTime - m_lastFrameTime;
  m_lastFrameTime = currentTime;

  Profiler::instance().beginFrame();

  // ── Fixed-timestep simulation (30 Hz) via SimulationLoop ────────────────
  {
    HeightQueryFn heightFn = nullptr;
    if (m_terrain) {
      heightFn = [this](float x, float z) {
        return m_terrain->GetHeightAt(x, z);
      };
    }

    SimulationContext simCtx;
    simCtx.scene            = &m_scene;
    simCtx.input            = m_input.get();
    simCtx.projectileSystem = &m_projectileSystem;
    simCtx.minionSystem     = &m_minionSystem;
    simCtx.structureSystem  = &m_structureSystem;
    simCtx.jungleSystem     = &m_jungleSystem;
    simCtx.autoAttackSystem = &m_autoAttackSystem;
    simCtx.particles        = m_particles.get();
    simCtx.heightFn         = heightFn;
    simCtx.gameTime         = &m_gameTime;
    simCtx.currentFrame     = m_currentFrame;
    simCtx.mobaMode         = m_mobaMode;
    simCtx.customMap        = m_customMap;

    simCtx.postTickCallback = [this]() {
        // ── Clean up bone slots for destroyed minion entities ─────────────
        {
          std::vector<uint32_t> deadEntities;
          for (auto &[eid, slot] : m_entityBoneSlot) {
            auto entity = static_cast<entt::entity>(eid);
            if (!m_scene.getRegistry().valid(entity)) {
              deadEntities.push_back(eid);
            }
          }
          for (uint32_t eid : deadEntities) {
            uint32_t slot = m_entityBoneSlot[eid];
            m_freeBoneSlots.push(slot);
            m_entityBoneSlot.erase(eid);
            m_minionOutputBuffers.erase(eid);
          }
        }

        // ── Assign render components to newly spawned minions ──────────────
        auto minionView = m_scene.getRegistry()
            .view<MinionTag, MinionIdentityComponent, TransformComponent>(
                entt::exclude<MeshComponent, GPUSkinnedMeshComponent, DynamicMeshComponent>);
        for (auto e : minionView) {
          auto &id = m_scene.getRegistry().get<MinionIdentityComponent>(e);

          glm::vec4 tint = (id.team == TeamID::Blue)
              ? glm::vec4(0.3f, 0.5f, 1.0f, 1.0f)
              : glm::vec4(1.0f, 0.3f, 0.3f, 1.0f);

          auto attachSkinned = [&](MinionTemplate &tmpl) {
            if (tmpl.staticSkinnedMeshIndex == UINT32_MAX) return;
            if (m_freeBoneSlots.empty()) {
              spdlog::warn("No free bone slots for minion — skipping GPU skin");
              return;
            }

            m_scene.getRegistry().emplace<GPUSkinnedMeshComponent>(e,
                GPUSkinnedMeshComponent{tmpl.staticSkinnedMeshIndex});

            uint32_t boneSlot = m_freeBoneSlots.front();
            m_freeBoneSlots.pop();
            uint32_t eid = static_cast<uint32_t>(e);
            m_entityBoneSlot[eid] = boneSlot;

            const auto& ssMesh = m_scene.getStaticSkinnedMesh(tmpl.staticSkinnedMeshIndex);
            uint32_t vtxCount = ssMesh.getVertexCount();
            Buffer outBuf(m_device->getAllocator(), 44ull * vtxCount,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VMA_MEMORY_USAGE_GPU_ONLY);
            m_minionOutputBuffers.emplace(eid, std::move(outBuf));

            ComputeSkinEntry entry{};
            entry.vertexCount = vtxCount;
            entry.entitySlot  = static_cast<uint32_t>(m_computeSkinEntries.size());
            entry.outputBuffer = Buffer(m_device->getAllocator(), 44ull * vtxCount,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       VMA_MEMORY_USAGE_GPU_ONLY);
            m_computeSkinEntries.push_back(std::move(entry));

            SkeletonComponent skelComp;
            skelComp.skeleton         = tmpl.skeleton;
            skelComp.skinVertices     = tmpl.skinVertices;
            skelComp.bindPoseVertices = tmpl.bindPoseVertices;
            m_scene.getRegistry().emplace<SkeletonComponent>(e, std::move(skelComp));

            AnimationComponent animComp;
            for (const auto &clip : tmpl.animClips)
              animComp.clips.push_back(clip);

            auto &skelRef = m_scene.getRegistry().get<SkeletonComponent>(e);
            animComp.player.setSkeleton(&skelRef.skeleton);
            if (!animComp.clips.empty()) {
              animComp.activeClipIndex = 0;
              animComp.player.setClip(&animComp.clips[0]);
            }
            m_scene.getRegistry().emplace<AnimationComponent>(e,
                std::move(animComp));

            m_scene.getRegistry().emplace<MaterialComponent>(e,
                MaterialComponent{tmpl.texIndex, m_minionFlatNorm,
                                  0.0f, 0.0f, 0.6f, 0.0f});
            m_scene.getRegistry().emplace<ColorComponent>(e, ColorComponent{tint});
            m_scene.getRegistry().emplace<TargetableComponent>(e,
                TargetableComponent{0.6f});

            auto &t = m_scene.getRegistry().get<TransformComponent>(e);
            t.scale      = glm::vec3(0.015f);
            t.rotation.x = glm::half_pi<float>();
          };

          if (id.type == MinionType::Melee && m_meleeMinionTemplate.loaded) {
            attachSkinned(m_meleeMinionTemplate);
          } else if (id.type == MinionType::Caster && m_casterMinionTemplate.loaded) {
            attachSkinned(m_casterMinionTemplate);
          } else {
            uint32_t typeIdx = static_cast<uint32_t>(id.type);
            MeshComponent mc{m_minionMeshIndices[typeIdx]};
            mc.localAABB = AABB{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
            m_scene.getRegistry().emplace<MeshComponent>(e, mc);
            m_scene.getRegistry().emplace<MaterialComponent>(e,
                MaterialComponent{m_minionDefaultTex, m_minionFlatNorm,
                                  0.0f, 0.0f, 0.6f, 0.0f});
            m_scene.getRegistry().emplace<ColorComponent>(e, ColorComponent{tint});
            auto &t = m_scene.getRegistry().get<TransformComponent>(e);
            switch (id.type) {
            case MinionType::Siege: t.scale = glm::vec3(0.9f); break;
            case MinionType::Super: t.scale = glm::vec3(1.2f); break;
            default: t.scale = glm::vec3(0.6f); break;
            }
            float hitRadius = (id.type == MinionType::Siege) ? 0.9f
                            : (id.type == MinionType::Super)  ? 1.2f : 0.6f;
            m_scene.getRegistry().emplace<TargetableComponent>(e,
                TargetableComponent{hitRadius});
          }
        }

        // Assign render components to newly spawned minion projectiles
        auto mProjNewView = m_scene.getRegistry()
            .view<MinionProjectileComponent, TransformComponent>(
                entt::exclude<MeshComponent>);
        for (auto e : mProjNewView) {
          MeshComponent mc{m_minionMeshIndices[0]};
          mc.localAABB = AABB{{-0.3f, -0.3f, -0.3f}, {0.3f, 0.3f, 0.3f}};
          m_scene.getRegistry().emplace<MeshComponent>(e, mc);
          m_scene.getRegistry().emplace<MaterialComponent>(
              e, MaterialComponent{m_minionDefaultTex, m_minionFlatNorm,
                                   0.0f, 0.0f, 0.3f, 0.5f});
          m_scene.getRegistry().emplace<ColorComponent>(
              e, ColorComponent{glm::vec4(1.0f, 0.9f, 0.5f, 1.0f)});
        }
    }; // end postTickCallback

    GLORY_PROFILE_SCOPE("Simulation");
    m_simLoop.tick(simCtx, deltaTime);

  }

  // Update particle emitter distance LOD (scale emission by camera proximity)
  if (m_particles) {
    glm::vec3 camPos = m_mobaMode ? m_isoCam.getPosition() : m_camera.getPosition();
    m_particles->setCameraPosition(camPos, 30.0f, 80.0f);
  }

  // Update isometric camera to follow the player character
  if (m_mobaMode) {
    auto charView =
        m_scene.getRegistry().view<TransformComponent, CharacterComponent>();
    for (auto entity : charView) {
      auto &transform = charView.get<TransformComponent>(entity);
      m_isoCam.setFollowTarget(transform.position);
      break; // follow first character
    }
  }

  // F1 toggles debug overlay, F2 toggles wireframe
  if (m_input->wasF1Pressed())
    m_overlay->toggleVisible();
  if (m_input->wasF2Pressed())
    m_wireframe = !m_wireframe;
  if (m_input->wasF3Pressed())
    m_showGrid = !m_showGrid;
  if (m_input->wasF4Pressed()) {
    m_mobaMode = !m_mobaMode;
    m_input->setCaptureEnabled(!m_mobaMode);
    spdlog::info("MOBA terrain mode: {}", m_mobaMode ? "ON" : "OFF");
  }
  if (m_input->wasYPressed()) {
    m_isoCam.toggleAttached();
    spdlog::info("Camera: {}", m_isoCam.isAttached() ? "ATTACHED" : "DETACHED");
  }

  // Right-click to move character (MOBA mode)
  if (m_mobaMode && m_input->wasRightClicked()) {
    glm::vec2 clickPos = m_input->getLastClickPos();
    float winW = static_cast<float>(m_swapchain->getExtent().width);
    float winH = static_cast<float>(m_swapchain->getExtent().height);

    glm::vec3 rayOrigin, rayDir;
    m_isoCam.screenToWorldRay(clickPos.x, clickPos.y, winW, winH, rayOrigin,
                              rayDir);

    if (std::abs(rayDir.y) > 0.0001f) {
      float t = -rayOrigin.y / rayDir.y;
      if (t > 0.0f) {
        glm::vec3 hitPos = rayOrigin + rayDir * t;

        // Refine: get actual terrain height at hit XZ and re-intersect
        if (m_terrain) {
          float terrainY = m_terrain->GetHeightAt(hitPos.x, hitPos.z);
          t = (terrainY - rayOrigin.y) / rayDir.y;
          hitPos = rayOrigin + rayDir * t;
          hitPos.y = terrainY;
        }

        // MOBA controls: Check if we clicked an enemy or ground
        entt::entity target = m_targetingSystem.pickTarget(m_scene.getRegistry(), rayOrigin, rayDir);
        bool isAttack = false;

        if (target != entt::null) {
            // Right-click on entity: set as auto-attack target
            if (m_playerEntity != entt::null && m_scene.getRegistry().all_of<PlayerTargetComponent>(m_playerEntity)) {
                auto &playerTarget = m_scene.getRegistry().get<PlayerTargetComponent>(m_playerEntity);
                playerTarget.targetEntity = target;
                isAttack = true;
                
                // Also get the target's position for the indicator
                if (m_scene.getRegistry().all_of<TransformComponent>(target)) {
                    hitPos = m_scene.getRegistry().get<TransformComponent>(target).position;
                }
            }
        } else {
            // Set target position on character entities
            auto charView = m_scene.getRegistry().view<CharacterComponent>();
            for (auto entity : charView) {
              auto &character = charView.get<CharacterComponent>(entity);
              character.targetPosition = hitPos;
              character.hasTarget = true;
            }

            // Cancel auto-attack target on right-click move
            if (m_playerEntity != entt::null &&
                m_scene.getRegistry().all_of<PlayerTargetComponent>(m_playerEntity)) {
              auto &playerTarget =
                  m_scene.getRegistry().get<PlayerTargetComponent>(m_playerEntity);
              playerTarget.targetEntity = entt::null;
            }
        }

        // Spawn click indicator at hit position
        m_clickIndicator = ClickIndicator{hitPos, 1.2f, 1.2f, isAttack};
      }
    }
  }

  // Left-click target selection (MOBA mode)
  if (m_mobaMode && m_input->wasLeftClicked()) {
    glm::vec2 clickPos = m_input->getLastLeftClickPos();
    float winW = static_cast<float>(m_swapchain->getExtent().width);
    float winH = static_cast<float>(m_swapchain->getExtent().height);

    glm::vec3 rayOrigin, rayDir;
    m_isoCam.screenToWorldRay(clickPos.x, clickPos.y, winW, winH, rayOrigin,
                              rayDir);

    auto hit = m_targetingSystem.pickTarget(m_scene.getRegistry(), rayOrigin,
                                            rayDir);
    if (m_playerEntity != entt::null &&
        m_scene.getRegistry().all_of<PlayerTargetComponent>(m_playerEntity)) {
      auto &playerTarget =
          m_scene.getRegistry().get<PlayerTargetComponent>(m_playerEntity);
      playerTarget.targetEntity = hit;
    }

    // Single-click selection: clear previous, select hit entity
    auto &reg = m_scene.getRegistry();
    reg.clear<SelectedComponent>();
    if (hit != entt::null) {
      reg.emplace_or_replace<SelectedComponent>(hit);
    }
  }

  // Marquee drag selection (MOBA mode)
  if (m_mobaMode && m_input->wasLeftDragReleased()) {
    auto &reg = m_scene.getRegistry();
    reg.clear<SelectedComponent>();

    float winW = static_cast<float>(m_swapchain->getExtent().width);
    float winH = static_cast<float>(m_swapchain->getExtent().height);
    float aspect = winW / winH;
    glm::mat4 proj = m_isoCam.getProjectionMatrix(aspect);
    proj[1][1] *= -1.0f; // undo Vulkan Y-flip
    glm::mat4 viewProj = proj * m_isoCam.getViewMatrix();

    auto [boxMin, boxMax] = m_input->getLeftDragRect();

    auto selectView = reg.view<TargetableComponent, TransformComponent>();
    for (auto entity : selectView) {
      auto *hp = reg.try_get<MinionHealthComponent>(entity);
      if (hp && hp->isDead) continue;

      auto &transform = selectView.get<TransformComponent>(entity);
      glm::vec4 clip = viewProj * glm::vec4(transform.position + glm::vec3(0, 1.0f, 0), 1.0f);
      if (clip.w <= 0.0f) continue;
      glm::vec3 ndc = glm::vec3(clip) / clip.w;
      float sx = (ndc.x * 0.5f + 0.5f) * winW;
      float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * winH;

      if (sx >= boxMin.x && sx <= boxMax.x && sy >= boxMin.y && sy <= boxMax.y) {
        reg.emplace_or_replace<SelectedComponent>(entity);
      }
    }
  }

  // Right-click ability casting checks
  if (m_mobaMode) {
    bool castQ = m_input->wasQPressed();
    bool castW = m_input->wasWAbilityPressed();
    bool castE = m_input->wasEPressed();
    bool castR = m_input->wasRPressed();

    if (castQ || castW || castE || castR) {
      glm::vec2 mousePos = m_input->getMousePos();
      float winW = static_cast<float>(m_swapchain->getExtent().width);
      float winH = static_cast<float>(m_swapchain->getExtent().height);

      glm::vec3 rayOrigin, rayDir;
      m_isoCam.screenToWorldRay(mousePos.x, mousePos.y, winW, winH, rayOrigin,
                                rayDir);
      glm::vec3 hitPos = rayOrigin;

      if (std::abs(rayDir.y) > 0.0001f) {
        float t = -rayOrigin.y / rayDir.y;
        if (t > 0.0f) {
          hitPos = rayOrigin + rayDir * t;
          if (m_terrain) {
            float terrainY = m_terrain->GetHeightAt(hitPos.x, hitPos.z);
            t = (terrainY - rayOrigin.y) / rayDir.y;
            hitPos = rayOrigin + rayDir * t;
            hitPos.y = terrainY;
          }
        }
      }

      auto view =
          m_scene.getRegistry().view<TransformComponent, CharacterComponent>();
      for (auto entity : view) {
        auto &tComp = view.get<TransformComponent>(entity);
        glm::vec3 direction = glm::normalize(hitPos - tComp.position);

        TargetInfo target;
        target.targetPosition = hitPos;
        target.direction = direction;
        target.targetEntity = entt::null;
        glm::vec3 origin = tComp.position + glm::vec3(0.0f, 1.2f, 0.0f);

        // Q = Fireball: fast narrow skillshot (glowing orange-red)
        if (castQ) {
          AbilitySystem::requestCast(m_scene.getRegistry(), entity,
                                     AbilitySlot::Q, target);
          m_projectileSystem.spawnSkillshot(
              m_scene, entity, "fire_mage_fireball", 1, origin, direction,
              1200.0f, 1100.0f, glm::vec4(1.8f, 0.4f, 0.05f, 1.0f), 0.35f);
        }
        // W = Flame Pillar: slower AoE ball (dark fiery orange)
        if (castW) {
          AbilitySystem::requestCast(m_scene.getRegistry(), entity,
                                     AbilitySlot::W, target);
          m_projectileSystem.spawnGroundAoE(
              m_scene, entity, "fire_mage_flame_pillar", 1, origin, hitPos,
              550.0f, 250.0f, glm::vec4(1.5f, 0.15f, 0.0f, 1.0f));
        }
        // E = Molten Shield: small teal orb pulsing outward
        if (castE) {
          AbilitySystem::requestCast(m_scene.getRegistry(), entity,
                                     AbilitySlot::E, target);
          m_projectileSystem.spawnSkillshot(
              m_scene, entity, "fire_mage_molten_shield", 1, origin, direction,
              300.0f, 150.0f, glm::vec4(0.05f, 2.4f, 2.4f, 1.0f), 0.6f);
        }
        if (castR)
          AbilitySystem::requestCast(m_scene.getRegistry(), entity,
                                     AbilitySlot::R, target);

        break;
      }
    }
  }

  // Tick click indicator
  if (m_clickIndicator) {
    m_clickIndicator->lifetime -= deltaTime;
    if (m_clickIndicator->lifetime <= 0.0f)
      m_clickIndicator.reset();
  }

  m_overlay->setWireframe(m_wireframe);

  // Finalize CPU profiler and expose results to overlay
  Profiler::instance().endFrame();
  {
    char cpuSummary[256] = {};
    int offset = 0;
    for (const auto& r : Profiler::instance().getResults()) {
      offset += std::snprintf(cpuSummary + offset,
                              sizeof(cpuSummary) - static_cast<size_t>(offset),
                              "%s %.2fms  ", r.name.c_str(), r.ms);
      if (offset >= static_cast<int>(sizeof(cpuSummary)) - 1) break;
    }
    m_overlay->setCpuTimings(cpuSummary);
  }

  // Feed debug overlay stats
  m_overlay->setFPS(deltaTime > 0.0f ? 1.0f / deltaTime : 0.0f);
  m_overlay->setFrameTime(deltaTime * 1000.0f);
  auto pos = m_mobaMode ? m_isoCam.getPosition() : m_camera.getPosition();
  m_overlay->setCameraPos(pos.x, pos.y, pos.z);
  {
    auto view = m_scene.getRegistry().view<TransformComponent, MeshComponent>();
    uint32_t count = 0;
    for ([[maybe_unused]] auto e : view)
      ++count;
    m_overlay->setEntityCount(count);
    m_overlay->setParticleCount(m_particles ? m_particles->getAliveCount() : 0);
    m_overlay->setMeshCount(static_cast<uint32_t>(
        m_scene.getRegistry().view<MeshComponent>().size()));
    m_overlay->setTextureCount(static_cast<uint32_t>(
        m_scene.getRegistry().view<MaterialComponent>().size()));
    // Draw call count and culled count are set during recordCommandBuffer
  }

  m_overlay->beginFrame();

  // Draw game HUD (uses ImGui background draw list — behind debug window,
  // on top of game scene).  Must be between NewFrame and Render.
  if (m_mobaMode && m_playerEntity != entt::null) {
    m_hud.setGameTime(m_gameTime);
    m_hud.draw(m_scene, m_playerEntity);
  }

  // Draw marquee selection box while dragging
  if (m_mobaMode && m_input->isLeftDragging()) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    auto [boxMin, boxMax] = m_input->getLeftDragRect();
    ImVec2 mn(boxMin.x, boxMin.y);
    ImVec2 mx(boxMax.x, boxMax.y);
    dl->AddRectFilled(mn, mx, IM_COL32(40, 200, 80, 50));
    dl->AddRect(mn, mx, IM_COL32(60, 255, 100, 200), 0.0f, 0, 1.5f);
  }

  // Draw world-space health bars above damaged minions
  if (m_mobaMode) {
    float aspect = static_cast<float>(m_swapchain->getExtent().width) /
                   static_cast<float>(m_swapchain->getExtent().height);
    // Use non-flipped projection for 2D overlay — the health bar code does
    // its own Y-flip when converting NDC → screen coords.
    glm::mat4 proj = m_isoCam.getProjectionMatrix(aspect);
    proj[1][1] *= -1.0f; // undo Vulkan Y-flip baked into getProjectionMatrix()
    glm::mat4 viewProj = proj * m_isoCam.getViewMatrix();
    m_minionHealthBars.draw(
        m_scene.getRegistry(), viewProj,
        static_cast<float>(m_swapchain->getExtent().width),
        static_cast<float>(m_swapchain->getExtent().height),
        m_isoCam.getPosition());
  }

  m_overlay->endFrame();

  // 1. Wait for this frame slot's previous work to finish
  VkFence fence = m_sync->getInFlightFence(m_currentFrame);
  vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

  // Readback GPU timestamps from the frame that just finished
  m_gpuProfiler->readback(m_currentFrame);
  {
    // Expose per-pass GPU times to the debug overlay
    char gpuSummary[256];
    m_gpuProfiler->fillSummary(gpuSummary, sizeof(gpuSummary));
    m_overlay->setGpuTimings(gpuSummary);
  }

  // 2. Acquire next swapchain image
  uint32_t imageIndex;
  VkSemaphore imgSem = m_sync->getImageAvailableSemaphore(m_currentFrame);
  VkResult result =
      vkAcquireNextImageKHR(device, m_swapchain->getSwapchain(), UINT64_MAX,
                            imgSem, VK_NULL_HANDLE, &imageIndex);

  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    recreateSwapchain();
    return;
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    throw std::runtime_error("Failed to acquire swapchain image");
  }

  vkResetFences(device, 1, &fence);

  // Tick texture streamer — polls transfer fences, submits graphics mip-gen,
  // and fires ready callbacks for fully uploaded textures.
  // Called here (after fence wait, before recording) so the graphics queue
  // is known to be idle for this frame slot.
  m_textureStreamer.tick();

  // 3. Record command buffer
  VkCommandBuffer cmd = m_sync->getCommandBuffer(m_currentFrame);
  vkResetCommandBuffer(cmd, 0);
  {
    GLORY_PROFILE_SCOPE("CmdRecord");
    recordCommandBuffer(cmd, imageIndex, deltaTime);
  }

  // 4. Submit
  VkSemaphore waitSems[] = {imgSem};
  VkPipelineStageFlags waitStages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  VkSemaphore sigSems[] = {m_sync->getRenderFinishedSemaphore(imageIndex)};

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = waitSems;
  submitInfo.pWaitDstStageMask = waitStages;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmd;
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = sigSems;

  VK_CHECK(vkQueueSubmit(m_device->getGraphicsQueue(), 1, &submitInfo, fence),
           "Failed to submit draw command buffer");

  // 5. Present
  VkSwapchainKHR swapchains[] = {m_swapchain->getSwapchain()};

  VkPresentInfoKHR presentInfo{};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = sigSems;
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = swapchains;
  presentInfo.pImageIndices = &imageIndex;

  result = vkQueuePresentKHR(m_device->getPresentQueue(), &presentInfo);

  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
      m_window.wasResized()) {
    m_window.resetResizedFlag();
    recreateSwapchain();
  } else if (result != VK_SUCCESS) {
    throw std::runtime_error("Failed to present swapchain image");
  }

  m_currentFrame = (m_currentFrame + 1) % Sync::MAX_FRAMES_IN_FLIGHT;
}

// ── Scene construction ──────────────────────────────────────────────────────
void Renderer::buildScene() {
  uint32_t cubeMesh =
      m_scene.addMesh(Model::createCube(*m_device, m_device->getAllocator()));
  uint32_t sphereMesh =
      m_scene.addMesh(Model::createSphere(*m_device, m_device->getAllocator()));
  // Per-type minion meshes for visual distinctiveness
  m_minionMeshIndices[0] = m_scene.addMesh(Model::createCapsule(*m_device, m_device->getAllocator(), 0.25f, 0.6f));
  m_minionMeshIndices[1] = m_scene.addMesh(Model::createCone(*m_device, m_device->getAllocator(), 0.25f, 0.7f));
  m_minionMeshIndices[2] = m_scene.addMesh(Model::createCylinder(*m_device, m_device->getAllocator(), 0.4f, 0.5f));
  m_minionMeshIndices[3] = m_scene.addMesh(Model::createIcosphere(*m_device, m_device->getAllocator(), 2, 0.5f));

  // Structure meshes
  m_towerMeshIndex = m_scene.addMesh(Model::createCylinder(*m_device, m_device->getAllocator(), 1.0f, 4.0f));
  m_towerTopMeshIndex = m_scene.addMesh(Model::createSphere(*m_device, m_device->getAllocator(), 16, 32));
  m_inhibitorMeshIndex = m_scene.addMesh(Model::createTorus(*m_device, m_device->getAllocator(), 1.5f, 0.4f));
  m_nexusMeshIndex = m_scene.addMesh(Model::createIcosphere(*m_device, m_device->getAllocator(), 3, 2.0f));

  // Monster meshes
  m_monsterSmallMeshIndex = sphereMesh;
  m_monsterBigMeshIndex = m_scene.addMesh(Model::createIcosphere(*m_device, m_device->getAllocator(), 2, 0.8f));
  m_monsterEpicMeshIndex = m_scene.addMesh(Model::createGear(*m_device, m_device->getAllocator(), 12, 1.2f, 0.3f, 0.5f));

  // LOD mesh variants (lower-detail versions of base meshes)
  uint32_t sphereLOD1 = m_scene.addMesh(Model::createSphere(
      *m_device, m_device->getAllocator(), 16, 32)); // medium
  uint32_t sphereLOD2 = m_scene.addMesh(
      Model::createSphere(*m_device, m_device->getAllocator(), 8, 16)); // low

  uint32_t defaultTex = m_scene.addTexture(Texture::createDefault(*m_device));
  m_minionDefaultTex = defaultTex;
  uint32_t checkerTex =
      m_scene.addTexture(Texture::createCheckerboard(*m_device));
  uint32_t flatNorm = m_scene.addTexture(Texture::createFlatNormal(*m_device));
  m_minionFlatNorm = flatNorm;
  uint32_t brickNorm =
      m_scene.addTexture(Texture::createBrickNormal(*m_device));
  uint32_t marbleTex = m_scene.addTexture(Texture::createMarble(*m_device));
  uint32_t woodTex = m_scene.addTexture(Texture::createWood(*m_device));
  uint32_t lavaTex = m_scene.addTexture(Texture::createLava(*m_device));
  uint32_t rockTex = m_scene.addTexture(Texture::createRock(*m_device));
  uint32_t metalTex =
      m_scene.addTexture(Texture::createBrushedMetal(*m_device));
  uint32_t tilesTex = m_scene.addTexture(Texture::createTiles(*m_device));
  uint32_t circuitTex = m_scene.addTexture(Texture::createCircuit(*m_device));
  uint32_t hexTex = m_scene.addTexture(Texture::createHexGrid(*m_device));
  uint32_t gradTex = m_scene.addTexture(Texture::createGradient(*m_device));
  uint32_t noiseTex = m_scene.addTexture(Texture::createNoise(*m_device));

  // Write all scene textures into the bindless descriptor array
  uint32_t texCount = static_cast<uint32_t>(m_scene.getTextures().size());
  for (uint32_t t = 0; t < texCount; ++t) {
    auto &tex = m_scene.getTexture(t);
    m_descriptors->writeBindlessTexture(t, tex.getImageView(),
                                        tex.getSampler());
  }
  m_descriptors->updateShadowMap(m_shadowMap->getDepthView(),
                                 m_shadowMap->getSampler());

  // ── Demo scene objects (only in non-MOBA mode) ────────────────────────────
  if (!m_mobaMode) {
  // Central rotating cube (slightly red-tinted)
  auto cube = m_scene.createEntity("Cube");
  m_scene.getRegistry().emplace<MeshComponent>(cube, MeshComponent{cubeMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      cube, MaterialComponent{defaultTex, flatNorm, 0.0f, 0.0f, 0.6f});
  m_scene.getRegistry().emplace<ColorComponent>(
      cube, ColorComponent{glm::vec4(1.0f, 0.85f, 0.85f, 1.0f)});
  m_scene.getRegistry().emplace<RotateComponent>(
      cube, RotateComponent{glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(45.0f)});

  // Shiny sphere — offset right, shows specular highlights well
  auto sphere1 = m_scene.createEntity("Sphere1");
  m_scene.getRegistry().emplace<MeshComponent>(sphere1,
                                               MeshComponent{sphereMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      sphere1, MaterialComponent{metalTex, flatNorm, 128.0f, 0.9f, 0.15f});
  m_scene.getRegistry().emplace<ColorComponent>(
      sphere1, ColorComponent{glm::vec4(0.9f, 0.95f, 1.0f, 1.0f)});
  auto &s1t = m_scene.getRegistry().get<TransformComponent>(sphere1);
  s1t.position = glm::vec3(3.0f, 0.0f, 0.0f);
  s1t.scale = glm::vec3(1.2f);
  {
    LODComponent lod{};
    lod.levelCount = 3;
    lod.levels[0] = {sphereMesh, 10.0f};   // < 10m: full detail
    lod.levels[1] = {sphereLOD1, 25.0f};   // < 25m: medium
    lod.levels[2] = {sphereLOD2, 1000.0f}; // > 25m: low
    m_scene.getRegistry().emplace<LODComponent>(sphere1, lod);
  }

  // Small orbiting sphere — circles the center
  auto orbiter = m_scene.createEntity("Orbiter");
  m_scene.getRegistry().emplace<MeshComponent>(orbiter,
                                               MeshComponent{sphereMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      orbiter, MaterialComponent{defaultTex, flatNorm, 0.0f, 0.7f, 0.3f});
  m_scene.getRegistry().emplace<ColorComponent>(
      orbiter, ColorComponent{glm::vec4(1.0f, 0.8f, 0.5f, 1.0f)});
  m_scene.getRegistry().emplace<RotateComponent>(
      orbiter,
      RotateComponent{glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(90.0f)});
  m_scene.getRegistry().emplace<OrbitComponent>(
      orbiter, OrbitComponent{glm::vec3(0.0f), 2.5f, 0.8f, 0.0f, 1.5f});
  auto &ot = m_scene.getRegistry().get<TransformComponent>(orbiter);
  ot.scale = glm::vec3(0.4f);

  // Green cube — offset left
  auto cube3 = m_scene.createEntity("GreenCube");
  m_scene.getRegistry().emplace<MeshComponent>(cube3, MeshComponent{cubeMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      cube3, MaterialComponent{defaultTex, flatNorm, 0.0f, 0.0f, 0.4f});
  m_scene.getRegistry().emplace<ColorComponent>(
      cube3, ColorComponent{glm::vec4(0.85f, 1.0f, 0.85f, 1.0f)});
  auto &t3 = m_scene.getRegistry().get<TransformComponent>(cube3);
  t3.position = glm::vec3(-2.5f, 0.0f, -1.0f);
  t3.scale = glm::vec3(0.7f);
  m_scene.getRegistry().emplace<RotateComponent>(
      cube3, RotateComponent{glm::vec3(1.0f, 1.0f, 0.0f), glm::radians(60.0f)});

  // Tall pillar
  auto pillar = m_scene.createEntity("Pillar");
  m_scene.getRegistry().emplace<MeshComponent>(pillar, MeshComponent{cubeMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      pillar, MaterialComponent{marbleTex, brickNorm, 0.0f, 0.0f, 0.7f});
  auto &tp = m_scene.getRegistry().get<TransformComponent>(pillar);
  tp.position = glm::vec3(-4.0f, 0.5f, 2.0f);
  tp.scale = glm::vec3(0.4f, 2.0f, 0.4f);

  // Pedestal sphere — on top of the pillar
  auto pedSphere = m_scene.createEntity("PedestalSphere");
  m_scene.getRegistry().emplace<MeshComponent>(pedSphere,
                                               MeshComponent{sphereMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      pedSphere, MaterialComponent{marbleTex, flatNorm, 96.0f, 0.0f, 0.3f});
  m_scene.getRegistry().emplace<RotateComponent>(
      pedSphere,
      RotateComponent{glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(20.0f)});
  auto &pst = m_scene.getRegistry().get<TransformComponent>(pedSphere);
  pst.position = glm::vec3(-4.0f, 2.0f, 2.0f);
  pst.scale = glm::vec3(0.6f);
  {
    LODComponent lod{};
    lod.levelCount = 3;
    lod.levels[0] = {sphereMesh, 10.0f};
    lod.levels[1] = {sphereLOD1, 25.0f};
    lod.levels[2] = {sphereLOD2, 1000.0f};
    m_scene.getRegistry().emplace<LODComponent>(pedSphere, lod);
  }

  // Floor terrain (heightmap with checkerboard)
  uint32_t terrainMesh = m_scene.addMesh(Model::createTerrain(
      *m_device, m_device->getAllocator(), 20.0f, 48, 1.0f));
  auto floor = m_scene.createEntity("Floor");
  m_scene.getRegistry().emplace<MeshComponent>(floor,
                                               MeshComponent{terrainMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      floor, MaterialComponent{rockTex, brickNorm, 8.0f, 0.0f, 0.85f});
  auto &ft = m_scene.getRegistry().get<TransformComponent>(floor);
  ft.position = glm::vec3(0.0f, -1.5f, 0.0f);
  ft.scale = glm::vec3(1.0f);

  // Bronze metallic torus — floating ring
  uint32_t torusMesh =
      m_scene.addMesh(Model::createTorus(*m_device, m_device->getAllocator()));
  auto torus = m_scene.createEntity("Torus");
  m_scene.getRegistry().emplace<MeshComponent>(torus, MeshComponent{torusMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      torus, MaterialComponent{lavaTex, flatNorm, 0.0f, 1.0f, 0.2f});
  m_scene.getRegistry().emplace<ColorComponent>(
      torus, ColorComponent{glm::vec4(0.85f, 0.55f, 0.25f, 1.0f)});
  m_scene.getRegistry().emplace<RotateComponent>(
      torus, RotateComponent{glm::vec3(0.3f, 1.0f, 0.1f), glm::radians(30.0f)});
  auto &tt = m_scene.getRegistry().get<TransformComponent>(torus);
  tt.position = glm::vec3(2.0f, 1.0f, -2.5f);
  tt.scale = glm::vec3(1.5f);

  // Ruby cone — offset far right
  uint32_t coneMesh =
      m_scene.addMesh(Model::createCone(*m_device, m_device->getAllocator()));
  auto cone = m_scene.createEntity("Cone");
  m_scene.getRegistry().emplace<MeshComponent>(cone, MeshComponent{coneMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      cone, MaterialComponent{defaultTex, flatNorm, 0.0f, 0.3f, 0.35f});
  m_scene.getRegistry().emplace<ColorComponent>(
      cone, ColorComponent{glm::vec4(0.9f, 0.2f, 0.25f, 1.0f)});
  m_scene.getRegistry().emplace<RotateComponent>(
      cone, RotateComponent{glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(40.0f)});
  auto &ct = m_scene.getRegistry().get<TransformComponent>(cone);
  ct.position = glm::vec3(4.5f, -0.5f, 1.5f);
  ct.scale = glm::vec3(1.0f, 2.0f, 1.0f);

  // Emerald cylinder — offset back-left
  uint32_t cylMesh = m_scene.addMesh(
      Model::createCylinder(*m_device, m_device->getAllocator()));
  auto cyl = m_scene.createEntity("Cylinder");
  m_scene.getRegistry().emplace<MeshComponent>(cyl, MeshComponent{cylMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      cyl, MaterialComponent{woodTex, flatNorm, 0.0f, 0.0f, 0.6f});
  m_scene.getRegistry().emplace<ColorComponent>(
      cyl, ColorComponent{glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
  m_scene.getRegistry().emplace<RotateComponent>(
      cyl, RotateComponent{glm::vec3(1.0f, 0.0f, 0.0f), glm::radians(25.0f)});
  auto &cyt = m_scene.getRegistry().get<TransformComponent>(cyl);
  cyt.position = glm::vec3(-3.0f, 0.5f, -3.0f);
  cyt.scale = glm::vec3(0.6f, 1.8f, 0.6f);

  // Amethyst capsule — front-right area
  uint32_t capsuleMesh = m_scene.addMesh(
      Model::createCapsule(*m_device, m_device->getAllocator()));
  auto capsule = m_scene.createEntity("Capsule");
  m_scene.getRegistry().emplace<MeshComponent>(capsule,
                                               MeshComponent{capsuleMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      capsule, MaterialComponent{defaultTex, flatNorm, 0.0f, 0.6f, 0.25f});
  m_scene.getRegistry().emplace<ColorComponent>(
      capsule, ColorComponent{glm::vec4(0.6f, 0.3f, 0.85f, 1.0f)});
  m_scene.getRegistry().emplace<RotateComponent>(
      capsule,
      RotateComponent{glm::vec3(0.0f, 1.0f, 0.2f), glm::radians(35.0f)});
  auto &capt = m_scene.getRegistry().get<TransformComponent>(capsule);
  capt.position = glm::vec3(1.5f, -0.2f, 3.0f);
  capt.scale = glm::vec3(0.8f, 1.5f, 0.8f);

  // Golden torus knot — ornamental centerpiece, floating above scene
  uint32_t knotMesh = m_scene.addMesh(
      Model::createTorusKnot(*m_device, m_device->getAllocator()));
  auto knot = m_scene.createEntity("TorusKnot");
  m_scene.getRegistry().emplace<MeshComponent>(knot, MeshComponent{knotMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      knot, MaterialComponent{tilesTex, flatNorm, 0.0f, 0.8f, 0.2f, 0.3f});
  m_scene.getRegistry().emplace<ColorComponent>(
      knot, ColorComponent{glm::vec4(1.0f, 0.85f, 0.4f, 1.0f)});
  m_scene.getRegistry().emplace<RotateComponent>(
      knot, RotateComponent{glm::vec3(0.2f, 1.0f, 0.3f), glm::radians(20.0f)});
  auto &kt = m_scene.getRegistry().get<TransformComponent>(knot);
  kt.position = glm::vec3(-1.0f, 2.0f, 0.0f);
  kt.scale = glm::vec3(1.2f);

  // Low-poly icosphere with circuit board texture — sci-fi artifact
  uint32_t icoMesh = m_scene.addMesh(
      Model::createIcosphere(*m_device, m_device->getAllocator(), 1));
  auto ico = m_scene.createEntity("Icosphere");
  m_scene.getRegistry().emplace<MeshComponent>(ico, MeshComponent{icoMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      ico, MaterialComponent{circuitTex, flatNorm, 0.0f, 0.5f, 0.4f, 0.2f});
  m_scene.getRegistry().emplace<ColorComponent>(
      ico, ColorComponent{glm::vec4(0.8f, 1.0f, 0.8f, 1.0f)});
  m_scene.getRegistry().emplace<RotateComponent>(
      ico, RotateComponent{glm::vec3(0.5f, 1.0f, 0.0f), glm::radians(15.0f)});
  auto &icot = m_scene.getRegistry().get<TransformComponent>(ico);
  icot.position = glm::vec3(5.0f, 1.0f, -1.0f);
  icot.scale = glm::vec3(0.7f);

  // Spring coil with hexagon grid texture
  uint32_t springMesh =
      m_scene.addMesh(Model::createSpring(*m_device, m_device->getAllocator()));
  auto spring = m_scene.createEntity("Spring");
  m_scene.getRegistry().emplace<MeshComponent>(spring,
                                               MeshComponent{springMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      spring, MaterialComponent{hexTex, flatNorm, 64.0f, 0.7f, 0.3f, 0.0f});
  m_scene.getRegistry().emplace<ColorComponent>(
      spring, ColorComponent{glm::vec4(0.7f, 0.9f, 1.0f, 1.0f)});
  m_scene.getRegistry().emplace<RotateComponent>(
      spring,
      RotateComponent{glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(30.0f)});
  auto &springt = m_scene.getRegistry().get<TransformComponent>(spring);
  springt.position = glm::vec3(-4.0f, 1.0f, 2.0f);
  springt.scale = glm::vec3(1.0f);

  // Mechanical gear with gradient texture
  uint32_t gearMesh =
      m_scene.addMesh(Model::createGear(*m_device, m_device->getAllocator()));
  auto gear = m_scene.createEntity("Gear");
  m_scene.getRegistry().emplace<MeshComponent>(gear, MeshComponent{gearMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      gear, MaterialComponent{gradTex, flatNorm, 32.0f, 0.8f, 0.35f, 0.0f});
  m_scene.getRegistry().emplace<ColorComponent>(
      gear, ColorComponent{glm::vec4(0.9f, 0.85f, 0.7f, 1.0f)});
  m_scene.getRegistry().emplace<RotateComponent>(
      gear, RotateComponent{glm::vec3(0.0f, 1.0f, 0.2f), glm::radians(45.0f)});
  auto &geart = m_scene.getRegistry().get<TransformComponent>(gear);
  geart.position = glm::vec3(4.0f, 1.5f, 3.0f);
  geart.scale = glm::vec3(0.8f);

  // Stone pyramid with noise texture
  uint32_t pyramidMesh =
      m_scene.addMesh(Model::createPyramid(*m_device, m_device->getAllocator()));
  auto pyramid = m_scene.createEntity("Pyramid");
  m_scene.getRegistry().emplace<MeshComponent>(pyramid,
                                               MeshComponent{pyramidMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      pyramid, MaterialComponent{noiseTex, flatNorm, 16.0f, 0.1f, 0.7f, 0.0f});
  m_scene.getRegistry().emplace<ColorComponent>(
      pyramid, ColorComponent{glm::vec4(0.85f, 0.8f, 0.65f, 1.0f)});
  auto &pyrt = m_scene.getRegistry().get<TransformComponent>(pyramid);
  pyrt.position = glm::vec3(-3.0f, 0.5f, -3.5f);
  pyrt.scale = glm::vec3(0.7f);

  // ── Rock LOD mesh variants (high/med/low poly icospheres) ──────────────
  uint32_t rockLOD0 = m_scene.addMesh(Model::createIcosphere(
      *m_device, m_device->getAllocator(), 2)); // 162 verts
  uint32_t rockLOD1 = m_scene.addMesh(Model::createIcosphere(
      *m_device, m_device->getAllocator(), 1)); // 42 verts
  uint32_t rockLOD2 = m_scene.addMesh(Model::createIcosphere(
      *m_device, m_device->getAllocator(), 0)); // 12 verts

  // ── Instanced rendering showcase: 200 scattered rocks (with LOD) ─────
  uint32_t rockMesh = rockLOD2; // base mesh (lowest detail for MeshComponent)
  {
    auto hash = [](int x, int y) -> float {
      int n = x * 73856093 ^ y * 19349663;
      n = (n << 13) ^ n;
      return static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) &
                                0x7FFFFFFF) /
             2147483648.0f;
    };

    for (int i = 0; i < 200; ++i) {
      float angle = hash(i, 0) * 6.28318f;
      float dist = 2.0f + hash(i, 1) * 12.0f;
      float px = dist * std::cos(angle);
      float pz = dist * std::sin(angle);
      float rockScale = 0.08f + hash(i, 2) * 0.18f;
      float rotY = hash(i, 3) * 6.28318f;
      float rotX = hash(i, 4) * 0.5f;

      auto rock = m_scene.createEntity("Rock");
      m_scene.getRegistry().emplace<MeshComponent>(
          rock, MeshComponent{rockMesh,
                              AABB{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}}});
      m_scene.getRegistry().emplace<MaterialComponent>(
          rock, MaterialComponent{rockTex, flatNorm, 0.0f, 0.0f, 0.85f, 0.0f});
      // Vary tint slightly per rock
      float tintR = 0.7f + hash(i, 5) * 0.3f;
      float tintG = 0.65f + hash(i, 6) * 0.3f;
      float tintB = 0.6f + hash(i, 7) * 0.25f;
      m_scene.getRegistry().emplace<ColorComponent>(
          rock, ColorComponent{glm::vec4(tintR, tintG, tintB, 1.0f)});
      auto &rt = m_scene.getRegistry().get<TransformComponent>(rock);
      rt.position = glm::vec3(px, -1.4f, pz);
      rt.rotation = glm::vec3(rotX, rotY, 0.0f);
      rt.scale = glm::vec3(rockScale);

      // LOD: high detail close, low detail far
      LODComponent lod{};
      lod.levelCount = 3;
      lod.levels[0] = {rockLOD0, 5.0f};    // < 5m: sub 2 (162 verts)
      lod.levels[1] = {rockLOD1, 15.0f};   // < 15m: sub 1 (42 verts)
      lod.levels[2] = {rockLOD2, 1000.0f}; // > 15m: sub 0 (12 verts)
      m_scene.getRegistry().emplace<LODComponent>(rock, lod);
    }
    spdlog::info("Added 200 instanced rocks (3-level LOD)");
  }

  // Primary white light (casts shadows)
  auto light0 = m_scene.createEntity("MainLight");
  auto &lt0 = m_scene.getRegistry().get<TransformComponent>(light0);
  lt0.position = glm::vec3(2.0f, 4.0f, 2.0f);
  m_scene.getRegistry().emplace<LightComponent>(
      light0, LightComponent{glm::vec3(1.0f, 0.95f, 0.9f), 1.2f,
                             LightComponent::Type::Point});

  // Red fill light — orbits the scene
  auto light1 = m_scene.createEntity("RedLight");
  auto &lt1 = m_scene.getRegistry().get<TransformComponent>(light1);
  lt1.position = glm::vec3(-3.0f, 2.0f, -1.0f);
  m_scene.getRegistry().emplace<LightComponent>(
      light1, LightComponent{glm::vec3(1.0f, 0.3f, 0.2f), 0.8f,
                             LightComponent::Type::Point});
  m_scene.getRegistry().emplace<OrbitComponent>(
      light1, OrbitComponent{glm::vec3(0.0f), 4.0f, 0.5f, 0.0f, 2.5f});

  // Blue rim light — orbits opposite direction
  auto light2 = m_scene.createEntity("BlueLight");
  auto &lt2 = m_scene.getRegistry().get<TransformComponent>(light2);
  lt2.position = glm::vec3(0.0f, 3.0f, -4.0f);
  m_scene.getRegistry().emplace<LightComponent>(
      light2, LightComponent{glm::vec3(0.2f, 0.4f, 1.0f), 0.6f,
                             LightComponent::Type::Point});
  m_scene.getRegistry().emplace<OrbitComponent>(
      light2, OrbitComponent{glm::vec3(0.0f), 5.0f, -0.35f, 2.0f, 3.0f});

  // Light gizmos — small bright cubes at each light position
  auto giz0 = m_scene.createEntity("Gizmo0");
  m_scene.getRegistry().emplace<MeshComponent>(giz0, MeshComponent{sphereMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      giz0, MaterialComponent{defaultTex, flatNorm, 0.0f, 0.0f, 0.1f, 5.0f});
  m_scene.getRegistry().emplace<ColorComponent>(
      giz0, ColorComponent{glm::vec4(8.0f, 7.6f, 7.2f, 1.0f)});
  auto &g0t = m_scene.getRegistry().get<TransformComponent>(giz0);
  g0t.position = glm::vec3(2.0f, 4.0f, 2.0f);
  g0t.scale = glm::vec3(0.12f);

  auto giz1 = m_scene.createEntity("Gizmo1");
  m_scene.getRegistry().emplace<MeshComponent>(giz1, MeshComponent{sphereMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      giz1, MaterialComponent{defaultTex, flatNorm, 0.0f, 0.0f, 0.1f, 5.0f});
  m_scene.getRegistry().emplace<ColorComponent>(
      giz1, ColorComponent{glm::vec4(8.0f, 2.4f, 1.6f, 1.0f)});
  auto &g1t = m_scene.getRegistry().get<TransformComponent>(giz1);
  g1t.position = glm::vec3(-3.0f, 2.0f, -1.0f);
  g1t.scale = glm::vec3(0.12f);
  m_scene.getRegistry().emplace<OrbitComponent>(
      giz1, OrbitComponent{glm::vec3(0.0f), 4.0f, 0.5f, 0.0f, 2.5f});

  auto giz2 = m_scene.createEntity("Gizmo2");
  m_scene.getRegistry().emplace<MeshComponent>(giz2, MeshComponent{sphereMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      giz2, MaterialComponent{defaultTex, flatNorm, 0.0f, 0.0f, 0.1f, 5.0f});
  m_scene.getRegistry().emplace<ColorComponent>(
      giz2, ColorComponent{glm::vec4(1.6f, 3.2f, 8.0f, 1.0f)});
  auto &g2t = m_scene.getRegistry().get<TransformComponent>(giz2);
  g2t.position = glm::vec3(0.0f, 3.0f, -4.0f);
  g2t.scale = glm::vec3(0.12f);
  m_scene.getRegistry().emplace<OrbitComponent>(
      giz2, OrbitComponent{glm::vec3(0.0f), 5.0f, -0.35f, 2.0f, 3.0f});

  } // end demo scene objects (!m_mobaMode)

  // ── MOBA mode: add a single overhead light for the scene ──────────────────
  if (m_mobaMode) {
    auto mobaLight = m_scene.createEntity("MobaLight");
    auto &lt = m_scene.getRegistry().get<TransformComponent>(mobaLight);
    lt.position = glm::vec3(100.0f, 20.0f, 100.0f); // Above map center
    // High intensity to compensate for quadratic attenuation at dist ~20
    m_scene.getRegistry().emplace<LightComponent>(
        mobaLight, LightComponent{glm::vec3(1.0f, 0.98f, 0.95f), 50.0f,
                                   LightComponent::Type::Point});

    // ── Flat LoL-style map ──────────────────────────────────────────────────
    uint32_t boxMesh = m_scene.addMesh(
        Model::createCube(*m_device, m_device->getAllocator()));

    // Place a flat axis-aligned tile (all map tiles need MapComponent to render)
    auto makeTile = [&](const std::string &name, glm::vec3 pos, glm::vec3 sz,
                        glm::vec4 col) {
      auto e = m_scene.createEntity(name);
      m_scene.getRegistry().emplace<MeshComponent>(e, MeshComponent{boxMesh});
      m_scene.getRegistry().emplace<MaterialComponent>(
          e, MaterialComponent{defaultTex, flatNorm, 0.0f, 0.0f, 1.0f});
      m_scene.getRegistry().emplace<ColorComponent>(e, ColorComponent{col});
      m_scene.getRegistry().emplace<MapComponent>(e);
      auto &t = m_scene.getRegistry().get<TransformComponent>(e);
      t.position = pos;
      t.scale    = sz;
      return e;
    };

    // Place a rotated strip (lane or river) aligned between two points
    auto makeStrip = [&](const std::string &name, glm::vec3 from, glm::vec3 to,
                         float width, float thickness, glm::vec4 col) {
      glm::vec3 d   = to - from;
      float     len = glm::length(d);
      glm::vec3 mid = (from + to) * 0.5f;
      auto e = m_scene.createEntity(name);
      m_scene.getRegistry().emplace<MeshComponent>(e, MeshComponent{boxMesh});
      m_scene.getRegistry().emplace<MaterialComponent>(
          e, MaterialComponent{defaultTex, flatNorm, 0.0f, 0.0f, 1.0f});
      m_scene.getRegistry().emplace<ColorComponent>(e, ColorComponent{col});
      m_scene.getRegistry().emplace<MapComponent>(e);
      auto &t = m_scene.getRegistry().get<TransformComponent>(e);
      t.position   = mid;
      t.scale      = glm::vec3(width, thickness, len);
      t.rotation.y = std::atan2(d.x, d.z);
      return e;
    };

    // Colors matching the reference image
    const glm::vec4 cBorder (0.04f, 0.04f, 0.04f, 1.f); // near-black border
    const glm::vec4 cJungle (0.13f, 0.18f, 0.08f, 1.f); // dark jungle fill
    const glm::vec4 cRiver  (0.18f, 0.50f, 0.78f, 1.f); // blue river
    const glm::vec4 cMid    (0.52f, 0.52f, 0.52f, 1.f); // mid lane — grey
    const glm::vec4 cTop    (0.35f, 0.55f, 0.28f, 1.f); // top lane — brighter green
    const glm::vec4 cBot    (0.58f, 0.38f, 0.18f, 1.f); // bot lane — earthy orange-brown

    // Corner positions
    const glm::vec3 blueCorner(20.f,  0.f, 180.f); // bottom-left
    const glm::vec3 redCorner (180.f, 0.f,  20.f); // top-right
    const glm::vec3 topCorner (20.f,  0.f,  20.f);
    const glm::vec3 botCorner (180.f, 0.f, 180.f);

    // 1. Outer dark border (underneath everything)
    makeTile("Border", {100, -0.8f, 100}, {214, 1.6f, 214}, cBorder);

    // 2. Jungle fill — the whole interior starts as dark jungle
    makeTile("Jungle", {100,  0.f,  100}, {196, 0.4f, 196}, cJungle);

    // 3. Three lanes in distinct colors on top of the jungle
    const float laneW = 11.f;
    const float laneH = 0.2f;

    // Mid lane (diagonal) — sandy tan
    makeStrip("LaneMid",   blueCorner, redCorner,  laneW, laneH, cMid);

    // Top lane — bright green (two segments: left edge + top edge)
    makeStrip("LaneTop_A", blueCorner, topCorner,  laneW, laneH, cTop);
    makeStrip("LaneTop_B", topCorner,  redCorner,  laneW, laneH, cTop);

    // Bot lane — earthy brown (two segments: bottom edge + right edge)
    makeStrip("LaneBot_A", blueCorner, botCorner,  laneW, laneH, cBot);
    makeStrip("LaneBot_B", botCorner,  redCorner,  laneW, laneH, cBot);

    // 4. River — blue diagonal strip on top of mid lane
    makeStrip("River", blueCorner, redCorner, 7.f, 0.35f, cRiver);

    m_glbMapLoaded = false;
    m_customMap    = true;
    spdlog::info("LoL-style flat map created (jungle + 3 coloured lanes + river)");
  }

  // ── Player character (scientist model at map center, click-to-move) ──────
  uint32_t charTex = defaultTex;
  auto character = m_scene.createEntity("PlayerCharacter");
  bool skinnedLoaded = false;

  try {
    // Load the base model (with skeleton + idle animation)
    std::string idlePath = std::string(MODEL_DIR) + "models/scientist/scientist.glb";
    auto skinnedData = Model::loadSkinnedFromGLB(
        *m_device, m_device->getAllocator(), idlePath,
        0.0f); // 39K verts — no decimation needed

    // Load textures from the idle model
    auto glbTextures = Model::loadGLBTextures(*m_device, idlePath);
    if (!glbTextures.empty()) {
      charTex = m_scene.addTexture(std::move(glbTextures[0]));
    }

    // Helper: build a StaticSkinnedMesh from a SkinnedData's first mesh
    auto buildSSMesh = [&](const auto& bpV, const auto& skinV2, const auto& idx) -> uint32_t {
      std::vector<SkinnedVertex> verts;
      verts.reserve(bpV.size());
      for (size_t vi = 0; vi < bpV.size(); ++vi) {
        SkinnedVertex sv{};
        sv.position = bpV[vi].position;
        sv.color    = bpV[vi].color;
        sv.normal   = bpV[vi].normal;
        sv.texCoord = bpV[vi].texCoord;
        sv.joints   = skinV2[vi].joints;
        sv.weights  = skinV2[vi].weights;
        verts.push_back(sv);
      }
      return m_scene.addStaticSkinnedMesh(
          StaticSkinnedMesh(*m_device, m_device->getAllocator(), verts, idx));
    };

    uint32_t ssIdx0 = buildSSMesh(skinnedData.bindPoseVertices[0],
                                   skinnedData.skinVertices[0],
                                   skinnedData.indices[0]);

    // Build SkeletonComponent (keeps skeleton + skin data for the player)
    SkeletonComponent skelComp;
    skelComp.skeleton = std::move(skinnedData.skeleton);
    skelComp.skinVertices     = std::move(skinnedData.skinVertices);
    skelComp.bindPoseVertices = std::move(skinnedData.bindPoseVertices);

    // Build AnimationComponent — clip 0 = idle
    // Always prefer the dedicated idle GLB (the base model may contain a
    // single-frame bind-pose "animation" that is NOT a real idle).
    AnimationComponent animComp;
    animComp.player.setSkeleton(&skelComp.skeleton);
    {
      bool idleLoaded = false;
      try {
        std::string idleAnimPath =
            std::string(MODEL_DIR) + "models/scientist/scientist_idle.glb";
        auto idleData = Model::loadSkinnedFromGLB(
            *m_device, m_device->getAllocator(), idleAnimPath, 0.0f);
        if (!idleData.animations.empty()) {
          animComp.clips.push_back(std::move(idleData.animations[0]));
          spdlog::info("Loaded idle animation: '{}'",
                       animComp.clips.back().name);
          idleLoaded = true;
        }
      } catch (const std::exception &e) {
        spdlog::warn("Could not load idle animation file: {}", e.what());
      }
      // Fall back to embedded animation only if no dedicated file
      if (!idleLoaded && !skinnedData.animations.empty()) {
        animComp.clips.push_back(std::move(skinnedData.animations[0]));
        spdlog::info("Using embedded idle animation (duration {:.3f}s)",
                     animComp.clips.back().duration);
      }
    }

    // Load walking animation — clip 1
    try {
      std::string walkPath =
          std::string(MODEL_DIR) + "models/scientist/scientist_walk.glb";
      auto walkData = Model::loadSkinnedFromGLB(
          *m_device, m_device->getAllocator(), walkPath, 0.0f);
      if (!walkData.animations.empty()) {
        animComp.clips.push_back(std::move(walkData.animations[0]));
        spdlog::info("Loaded walk animation: '{}'",
                     animComp.clips.back().name);
      }
    } catch (const std::exception &e) {
      spdlog::warn("Could not load walk animation: {}", e.what());
    }

    // Load auto-attack animation — clip 2
    try {
      std::string atkPath =
          std::string(MODEL_DIR) + "models/scientist/scientist_auto_attack.glb";
      auto atkData = Model::loadSkinnedFromGLB(
          *m_device, m_device->getAllocator(), atkPath, 0.0f);
      if (!atkData.animations.empty()) {
        animComp.clips.push_back(std::move(atkData.animations[0]));
        spdlog::info("Loaded auto-attack animation: '{}'",
                     animComp.clips.back().name);
      }
    } catch (const std::exception &e) {
      spdlog::warn("Could not load auto-attack animation: {}", e.what());
    }

    // Set initial clip (idle)
    if (!animComp.clips.empty()) {
      animComp.activeClipIndex = 0;
      animComp.player.setClip(&animComp.clips[0]);
    }

    // Attach components — skeleton must be emplaced before we take its address
    m_scene.getRegistry().emplace<SkeletonComponent>(character,
                                                     std::move(skelComp));
    m_scene.getRegistry().emplace<GPUSkinnedMeshComponent>(
        character, GPUSkinnedMeshComponent{ssIdx0});

    // Single LOD (39K verts is already lightweight)
    {
      SkinnedLODComponent slod{};
      slod.levelCount = 1;
      slod.levels[0] = {ssIdx0, 999.0f};
      m_scene.getRegistry().emplace<SkinnedLODComponent>(character, slod);
    }
    // Re-acquire skeleton pointer after emplace (move may invalidate)
    auto &skelRef = m_scene.getRegistry().get<SkeletonComponent>(character);
    animComp.player.setSkeleton(&skelRef.skeleton);
    m_scene.getRegistry().emplace<AnimationComponent>(character,
                                                      std::move(animComp));

    m_scene.getRegistry().emplace<MaterialComponent>(
        character, MaterialComponent{charTex, flatNorm, 0.0f, 0.0f, 0.7f});
    m_scene.getRegistry().emplace<ColorComponent>(
        character, ColorComponent{glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});

    skinnedLoaded = true;
    spdlog::info(
        "Skinned character loaded with {} animation clip(s)",
        m_scene.getRegistry().get<AnimationComponent>(character).clips.size());

  } catch (const std::exception &e) {
    spdlog::error("Failed to load skinned GLB: {} – falling back to capsule",
                  e.what());
    uint32_t charMesh = m_scene.addMesh(
        Model::createCapsule(*m_device, m_device->getAllocator()));
    m_scene.getRegistry().emplace<MeshComponent>(character,
                                                 MeshComponent{charMesh});
    m_scene.getRegistry().emplace<MaterialComponent>(
        character, MaterialComponent{charTex, flatNorm, 0.0f, 0.0f, 0.7f});
    m_scene.getRegistry().emplace<ColorComponent>(
        character, ColorComponent{glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
  }

  m_scene.getRegistry().emplace<CharacterComponent>(character);
  auto &charT = m_scene.getRegistry().get<TransformComponent>(character);
  charT.position = glm::vec3(100.0f, 0.0f, 100.0f);
  // Armature has scale 0.01 → skinning inflates ~100×. Entity scale 0.015
  // gives final height ≈ 0.015 * 100 * 0.98 ≈ 1.5 world units (good MOBA size).
  charT.scale      = glm::vec3(0.015f);
  charT.rotation.x = glm::half_pi<float>();     // +90° X (Blender Z-up fix)
  spdlog::info("Character transform: pos({},{},{}) scale({:.3f}) rotation.x=+90deg",
               charT.position.x, charT.position.y, charT.position.z,
               charT.scale.x);

  // ── Attach Ability system components to character ────────────────────────
  m_scene.getRegistry().emplace<AbilityInputComponent>(character);
  m_scene.getRegistry().emplace<StatusEffectsComponent>(character);
  auto &combatStats =
      m_scene.getRegistry().emplace<CombatStatsComponent>(character);
  combatStats.currentResource = 1000.0f; // Give plenty of mana
  combatStats.maxResource = 1000.0f;
  combatStats.resourceRegen = 20.0f;
  combatStats.abilityPower = 100.0f;

  auto &book = m_scene.getRegistry().emplace<AbilityBookComponent>(character);

  // Load sample abilities into central database
  auto &db = AbilityDatabase::get();
  if (!db.find("fire_mage_fireball")) {
    try {
      db.loadFromFile(std::string(ABILITY_DATA_DIR) + "fire_mage_fireball.json");
      db.loadFromFile(std::string(ABILITY_DATA_DIR) + "fire_mage_flame_pillar.json");
      db.loadFromFile(std::string(ABILITY_DATA_DIR) + "fire_mage_molten_shield.json");
    } catch (const std::exception &e) {
      spdlog::error("Failed to load sample abilities: {}", e.what());
    }
  }

  book.get(AbilitySlot::Q).def = db.find("fire_mage_fireball");
  book.get(AbilitySlot::Q).level = 1;
  book.get(AbilitySlot::W).def = db.find("fire_mage_flame_pillar");
  book.get(AbilitySlot::W).level = 1;
  book.get(AbilitySlot::E).def = db.find("fire_mage_molten_shield");
  book.get(AbilitySlot::E).level = 1;

  // Cache mesh index and init ProjectileSystem so projectiles can reuse the
  // same mesh
  m_sphereMeshIndex = sphereMesh;
  m_projectileSystem.init(m_scene, sphereMesh, defaultTex, flatNorm);

  // ── Player targeting & auto-attack ──────────────────────────────────────
  m_playerEntity = character;
  m_scene.getRegistry().emplace<PlayerTargetComponent>(character);
  m_scene.getRegistry().emplace<AutoAttackComponent>(character);
  m_hud.init(static_cast<float>(m_swapchain->getExtent().width),
             static_cast<float>(m_swapchain->getExtent().height));

  // ── Allocate compute-skin output buffers for all registered skinned entities ──
  // Each GPUSkinnedMeshComponent entity gets one persistent device-local buffer.
  {
    m_computeSkinEntries.clear();
    auto gpuView = m_scene.getRegistry()
        .view<GPUSkinnedMeshComponent>();
    for (auto entity : gpuView) {
      auto& gpuComp = gpuView.get<GPUSkinnedMeshComponent>(entity);
      const auto& ssMesh = m_scene.getStaticSkinnedMesh(gpuComp.staticSkinnedMeshIndex);
      ComputeSkinEntry entry{};
      entry.vertexCount  = ssMesh.getVertexCount();
      entry.entitySlot   = static_cast<uint32_t>(m_computeSkinEntries.size());

      // Pre-skinned vertex stride = 44 bytes
      entry.outputBuffer = Buffer(m_device->getAllocator(), 44ull * entry.vertexCount,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VMA_MEMORY_USAGE_GPU_ONLY);
      m_computeSkinEntries.push_back(std::move(entry));
    }    if (!m_computeSkinEntries.empty()) {
      spdlog::info("ComputeSkinner: allocated {} output buffers for skinned entities",
                   m_computeSkinEntries.size());
    }
  }

  // ── Minion NPC system (MOBA mode) ─────────────────────────────────────────
  if (m_mobaMode) {
    auto minionCfg = MinionConfigLoader::Load(
        std::string(MAP_DATA_DIR) + "../config/");
    m_minionSystem.init(minionCfg, m_mapData);

    // ── Load skinned minion templates (used for Melee & Caster) ──────────────
    auto loadMinionTemplate = [&](MinionTemplate &tmpl,
                                  const std::string &walkPath,
                                  const std::string &atkPath) {
      try {
        auto walkData = Model::loadSkinnedFromGLB(
            *m_device, m_device->getAllocator(), walkPath, 0.6f); // 60% decimation
        tmpl.skeleton          = std::move(walkData.skeleton);
        tmpl.bindPoseVertices  = std::move(walkData.bindPoseVertices);
        tmpl.skinVertices      = std::move(walkData.skinVertices);
        tmpl.indices           = std::move(walkData.indices);
        if (!walkData.animations.empty())
          tmpl.animClips.push_back(std::move(walkData.animations[0])); // clip 0 = walk

        try {
          auto atkData = Model::loadSkinnedFromGLB(
              *m_device, m_device->getAllocator(), atkPath, 0.6f); // 60% decimation
          if (!atkData.animations.empty())
            tmpl.animClips.push_back(std::move(atkData.animations[0])); // clip 1 = attack
        } catch (const std::exception &e) {
          spdlog::warn("Could not load minion attack anim '{}': {}", atkPath, e.what());
        }

        auto texs = Model::loadGLBTextures(*m_device, walkPath);
        tmpl.texIndex = texs.empty() ? m_minionDefaultTex
                                     : m_scene.addTexture(std::move(texs[0]));

        // Build a shared StaticSkinnedMesh for GPU skinning (uploaded once to GPU)
        if (!tmpl.bindPoseVertices.empty() && !tmpl.skinVertices.empty()) {
          std::vector<SkinnedVertex> verts;
          const auto& bpV   = tmpl.bindPoseVertices[0];
          const auto& skinV = tmpl.skinVertices[0];
          verts.reserve(bpV.size());
          for (size_t vi = 0; vi < bpV.size(); ++vi) {
            SkinnedVertex sv{};
            sv.position = bpV[vi].position;
            sv.color    = bpV[vi].color;
            sv.normal   = bpV[vi].normal;
            sv.texCoord = bpV[vi].texCoord;
            sv.joints   = skinV[vi].joints;
            sv.weights  = skinV[vi].weights;
            verts.push_back(sv);
          }
          tmpl.staticSkinnedMeshIndex = m_scene.addStaticSkinnedMesh(
              StaticSkinnedMesh(*m_device, m_device->getAllocator(),
                                verts, tmpl.indices[0]));
          spdlog::info("Built shared StaticSkinnedMesh for '{}' ({} verts)",
                       walkPath, bpV.size());
        }

        tmpl.loaded = true;
        spdlog::info("Loaded minion template '{}' ({} clips)", walkPath,
                     tmpl.animClips.size());
      } catch (const std::exception &e) {
        spdlog::warn("Could not load minion template '{}': {}", walkPath, e.what());
      }
    };

    loadMinionTemplate(m_meleeMinionTemplate,
        std::string(MODEL_DIR) + "models/melee_minion/melee_minion_walking.glb",
        std::string(MODEL_DIR) + "models/melee_minion/melee_minion_attack1.glb");

    loadMinionTemplate(m_casterMinionTemplate,
        std::string(MODEL_DIR) + "models/caster_minion/caster_walking.glb",
        std::string(MODEL_DIR) + "models/caster_minion/caster_attacking.glb");

    // ── Initialize bone slot pool for GPU-skinned minions ──────────────────
    // Slot 0 is used by the player character; slots 1..127 available for minions
    while (!m_freeBoneSlots.empty()) m_freeBoneSlots.pop(); // clear
    for (uint32_t s = 1; s < Descriptors::MAX_SKINNED_CHARS; ++s)
      m_freeBoneSlots.push(s);
    m_entityBoneSlot.clear();
    m_minionOutputBuffers.clear();

    if (!m_customMap) {
      auto structCfg = StructureConfigLoader::Load(
          std::string(MAP_DATA_DIR) + "../config/");
      auto jungleCfg = JungleConfigLoader::Load(
          std::string(MAP_DATA_DIR) + "../config/");

      auto heightFn = [this](float x, float z) { return m_terrain ? m_terrain->GetHeightAt(x, z) : 0.0f; };
      m_structureSystem.init(structCfg, m_mapData, m_scene.getRegistry(), heightFn);
      m_jungleSystem.init(jungleCfg, m_mapData, m_scene.getRegistry(), heightFn);

      // Assign meshes to structures
      auto towerView = m_scene.getRegistry().view<TowerTag, TransformComponent>(entt::exclude<MeshComponent>);
      for (auto e : towerView) {
          auto &id = m_scene.getRegistry().get<StructureIdentityComponent>(e);
          m_scene.getRegistry().emplace<MeshComponent>(e, MeshComponent{m_towerMeshIndex});
          m_scene.getRegistry().emplace<MaterialComponent>(e, MaterialComponent{m_minionDefaultTex, m_minionFlatNorm, 0, 0, 0.4f, 0.0f});
          glm::vec4 tint = (id.team == TeamID::Blue) ? glm::vec4(0.2f, 0.4f, 0.9f, 1.0f) : glm::vec4(0.9f, 0.2f, 0.2f, 1.0f);
          m_scene.getRegistry().emplace<ColorComponent>(e, ColorComponent{tint});
          m_scene.getRegistry().emplace<TargetableComponent>(e, TargetableComponent{2.0f});
      }

      auto inhibView = m_scene.getRegistry().view<InhibitorTag, TransformComponent>(entt::exclude<MeshComponent>);
      for (auto e : inhibView) {
          auto &id = m_scene.getRegistry().get<StructureIdentityComponent>(e);
          m_scene.getRegistry().emplace<MeshComponent>(e, MeshComponent{m_inhibitorMeshIndex});
          m_scene.getRegistry().emplace<MaterialComponent>(e, MaterialComponent{m_minionDefaultTex, m_minionFlatNorm, 0, 0, 0.4f, 0.0f});
          glm::vec4 tint = (id.team == TeamID::Blue) ? glm::vec4(0.2f, 0.4f, 0.9f, 1.0f) : glm::vec4(0.9f, 0.2f, 0.2f, 1.0f);
          m_scene.getRegistry().emplace<ColorComponent>(e, ColorComponent{tint});
          m_scene.getRegistry().emplace<TargetableComponent>(e, TargetableComponent{2.0f});
      }

      auto nexusView = m_scene.getRegistry().view<NexusTag, TransformComponent>(entt::exclude<MeshComponent>);
      for (auto e : nexusView) {
          auto &id = m_scene.getRegistry().get<StructureIdentityComponent>(e);
          m_scene.getRegistry().emplace<MeshComponent>(e, MeshComponent{m_nexusMeshIndex});
          m_scene.getRegistry().emplace<MaterialComponent>(e, MaterialComponent{m_minionDefaultTex, m_minionFlatNorm, 0, 0, 0.4f, 0.0f});
          glm::vec4 tint = (id.team == TeamID::Blue) ? glm::vec4(0.2f, 0.4f, 0.9f, 1.0f) : glm::vec4(0.9f, 0.2f, 0.2f, 1.0f);
          m_scene.getRegistry().emplace<ColorComponent>(e, ColorComponent{tint});
          m_scene.getRegistry().emplace<TargetableComponent>(e, TargetableComponent{3.0f});
      }

      auto monsterView = m_scene.getRegistry().view<JungleMonsterTag, TransformComponent>(entt::exclude<MeshComponent>);
      for (auto e : monsterView) {
          auto &id = m_scene.getRegistry().get<MonsterIdentityComponent>(e);
          uint32_t mesh = id.isBigMonster ? m_monsterBigMeshIndex : m_monsterSmallMeshIndex;
          if (id.campType == CampType::Dragon || id.campType == CampType::Baron || id.campType == CampType::Herald) {
              mesh = m_monsterEpicMeshIndex;
          }
          m_scene.getRegistry().emplace<MeshComponent>(e, MeshComponent{mesh});
          m_scene.getRegistry().emplace<MaterialComponent>(e, MaterialComponent{m_minionDefaultTex, m_minionFlatNorm, 0, 0, 0.4f, 0.0f});
          m_scene.getRegistry().emplace<ColorComponent>(e, ColorComponent{{0.4f, 0.8f, 0.2f, 1.0f}});
          m_scene.getRegistry().emplace<TargetableComponent>(e, TargetableComponent{1.5f});
      }
    } // !m_customMap

    m_gameTime = 0.0f;
  }

  spdlog::info(
      "Scene built: spheres + cubes, 3 lights, orbiter, sky gradient + bloom");
}

// ── Light view-projection matrix ────────────────────────────────────────────
glm::mat4 Renderer::computeLightVP() const {
  glm::vec3 lightPos(2.0f, 4.0f, 2.0f);
  glm::vec3 lightColor;
  m_scene.getFirstLight(lightPos, lightColor);

  glm::vec3 shadowTarget(0.0f);
  float shadowExtent = 10.0f;
  float shadowFar    = 30.0f;

  if (m_mobaMode) {
    // ── Fit shadow frustum tightly to camera view frustum ──────────────────
    // Transform the 8 camera frustum corners into light space and take their
    // AABB — this eliminates wasted shadow map texels outside the visible area.
    shadowTarget = m_isoCam.getTarget();
    glm::vec3 lightDir = glm::normalize(lightPos - shadowTarget);
    lightPos = shadowTarget + lightDir * 30.0f;

    glm::mat4 lightView = glm::lookAt(lightPos, shadowTarget, glm::vec3(0, 1, 0));

    float aspect = 1.0f;
    {
      VkExtent2D ext = m_swapchain->getExtent();
      if (ext.height > 0)
        aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);
    }
    glm::mat4 camVP    = m_isoCam.getProjectionMatrix(aspect) * m_isoCam.getViewMatrix();
    glm::mat4 invCamVP = glm::inverse(camVP);

    static const glm::vec4 ndcCorners[8] = {
      {-1,-1, 0, 1}, { 1,-1, 0, 1}, { 1, 1, 0, 1}, {-1, 1, 0, 1},
      {-1,-1, 1, 1}, { 1,-1, 1, 1}, { 1, 1, 1, 1}, {-1, 1, 1, 1}
    };

    glm::vec3 lsMin( 1e9f);
    glm::vec3 lsMax(-1e9f);
    for (const auto& c : ndcCorners) {
      glm::vec4 world = invCamVP * c;
      world /= world.w;
      glm::vec4 ls = lightView * world;
      lsMin = glm::min(lsMin, glm::vec3(ls));
      lsMax = glm::max(lsMax, glm::vec3(ls));
    }

    float margin = 4.0f; // small padding so shadow edges don't clip
    glm::mat4 lightProj = glm::ortho(
        lsMin.x - margin, lsMax.x + margin,
        lsMin.y - margin, lsMax.y + margin,
        -lsMax.z - margin, -lsMin.z + margin);
    lightProj[1][1] *= -1.0f;
    return lightProj * lightView;
  }

  glm::mat4 lightView =
      glm::lookAt(lightPos, shadowTarget, glm::vec3(0.0f, 1.0f, 0.0f));
  glm::mat4 lightProj = glm::ortho(-shadowExtent, shadowExtent,
                                     -shadowExtent, shadowExtent,
                                     0.1f, shadowFar);
  lightProj[1][1] *= -1.0f;
  return lightProj * lightView;
}

// ── Swapchain recreation ────────────────────────────────────────────────────
void Renderer::recreateSwapchain() {
  VkExtent2D extent = m_window.getExtent();
  while (extent.width == 0 || extent.height == 0) {
    extent = m_window.getExtent();
    glfwWaitEvents();
  }

  vkDeviceWaitIdle(m_device->getDevice());

  m_swapchain->recreate(extent);
  m_postProcess->recreate(*m_swapchain);
  if (m_bloom && m_ssao) {
    m_bloom->recreate(*m_swapchain, m_postProcess->getHDRImageView());
    m_ssao->recreate(*m_swapchain, m_postProcess->getHDRDepthView());
    m_postProcess->updateBloomDescriptor(m_bloom->getOutputImageView());
    m_postProcess->updateSSAODescriptor(m_ssao->getOutputImageView());
  } else {
    // MOBA mode — re-bind dummy descriptors (they don't change on swapchain resize)
    m_postProcess->updateBloomDescriptor(m_postProcess->getDummyBloomImageView());
    m_postProcess->updateSSAODescriptor(m_postProcess->getDummySSAOImageView());
  }
  m_postProcess->updateDepthDescriptor(m_postProcess->getHDRDepthView());
  m_pipeline->recreateFramebuffers(
      *m_swapchain); // no-op for external render pass
  m_sync->recreateRenderFinishedSemaphores(m_swapchain->getImageCount());
  m_overlay->onSwapchainRecreate();
  m_hud.resize(static_cast<float>(m_swapchain->getExtent().width),
               static_cast<float>(m_swapchain->getExtent().height));

  spdlog::info("Swapchain recreation complete");
}

// ── Command recording — 3-pass pipeline ─────────────────────────────────────
void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex,
                                   float deltaTime) {
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo),
           "Failed to begin recording command buffer");

  // Reset timestamp query pool for this frame slot, then start profiling
  m_gpuProfiler->resetPool(cmd, m_currentFrame);

  auto extent = m_swapchain->getExtent();
  float aspect =
      static_cast<float>(extent.width) / static_cast<float>(extent.height);
  glm::mat4 lightVP = computeLightVP();

  glm::mat4 viewMat = m_camera.getViewMatrix();
  glm::mat4 projMat = m_camera.getProjectionMatrix(aspect);
  Frustum frustum;
  frustum.update(projMat * viewMat);

  uint32_t drawCalls = 0;
  uint32_t culled = 0;
  uint32_t visibleInstances = 0;
  uint32_t indirectCmdCount = 0;

  // Update shadow map light matrix UBO
  m_shadowMap->updateLightMatrix(lightVP);

  // ════════════════════════════════════════════════════════════════════════
  // Cascade Shadow Map pass (MOBA mode only)
  // 3 sub-passes, each writing to one layer of the shadow array.
  // This replaces the single ortho shadow with 3 tight cascades.
  // ════════════════════════════════════════════════════════════════════════
  if (m_mobaMode && m_cascadeShadow->isInitialised()) {
    // Compute cascade VP matrices from the isometric camera params
    glm::vec3 lightDir(0);
    glm::vec3 dummy;
    m_scene.getFirstLight(lightDir, dummy);
    lightDir = glm::normalize(lightDir - m_isoCam.getTarget());

    auto csmVPs = m_cascadeShadow->computeCascadeVPs(
        m_isoCam.getViewMatrix(),
        glm::radians(45.0f), aspect, 0.1f, 120.0f,
        lightDir);
    m_cascadeShadow->updateLightMatrices(csmVPs);

    // Record one render pass per cascade
    for (uint32_t ci = 0; ci < CascadeShadow::CASCADE_COUNT; ++ci) {
      VkClearValue depthClear{};
      depthClear.depthStencil.depth = 1.0f;

      VkRenderPassBeginInfo rpInfo{};
      rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      rpInfo.renderPass        = m_cascadeShadow->getRenderPass();
      rpInfo.framebuffer       = m_cascadeShadow->getFramebuffer(ci);
      rpInfo.renderArea.extent = {CascadeShadow::SHADOW_MAP_SIZE,
                                   CascadeShadow::SHADOW_MAP_SIZE};
      rpInfo.clearValueCount   = 1;
      rpInfo.pClearValues      = &depthClear;
      vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_cascadeShadow->getPipeline());

      VkViewport vp{};
      vp.width    = static_cast<float>(CascadeShadow::SHADOW_MAP_SIZE);
      vp.height   = static_cast<float>(CascadeShadow::SHADOW_MAP_SIZE);
      vp.maxDepth = 1.0f;
      vkCmdSetViewport(cmd, 0, 1, &vp);
      VkRect2D sc{};
      sc.extent = {CascadeShadow::SHADOW_MAP_SIZE, CascadeShadow::SHADOW_MAP_SIZE};
      vkCmdSetScissor(cmd, 0, 1, &sc);
      vkCmdSetDepthBias(cmd, 1.25f, 0.0f, 1.75f);

      // Bind CSM descriptor set (UBO at binding 1)
      VkDescriptorSet csmDescSet = m_cascadeShadow->getDescSet();
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_cascadeShadow->getPipelineLayout(), 0, 1,
                              &csmDescSet, 0, nullptr);

      // Draw static mesh entities
      Frustum csmFrustum;
      csmFrustum.update(csmVPs[ci]);
      auto csmView = m_scene.getRegistry()
          .view<TransformComponent, MeshComponent>();
      for (auto entity : csmView) {
        auto &tc = csmView.get<TransformComponent>(entity);
        auto &mc = csmView.get<MeshComponent>(entity);
        glm::mat4 model = tc.getModelMatrix();
        AABB worldAABB = mc.localAABB.transformed(model);
        if (!csmFrustum.isVisible(worldAABB)) continue;
        vkCmdPushConstants(cmd, m_cascadeShadow->getPipelineLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model);
        m_scene.getMesh(mc.meshIndex).draw(cmd);
      }

      vkCmdEndRenderPass(cmd);
    }

    // Barrier: CSM depth write → shader read
    VkImageMemoryBarrier csmBarrier{};
    csmBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    csmBarrier.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    csmBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    csmBarrier.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    csmBarrier.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    csmBarrier.image               = m_cascadeShadow->getArrayView() == VK_NULL_HANDLE
                                     ? VK_NULL_HANDLE : VK_NULL_HANDLE; // use below
    csmBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    csmBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    csmBarrier.subresourceRange    = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0,
                                       CascadeShadow::CASCADE_COUNT};
    // The image handle is managed by CascadeShadow privately; we need the
    // barrier only on the VkImage.  We skip the barrier here and rely on the
    // subpass dependency declared in CascadeShadow::createRenderPass which
    // already transitions to DEPTH_STENCIL_READ_ONLY_OPTIMAL with the correct
    // access mask.  No explicit vkCmdPipelineBarrier needed.
  }

  // ════════════════════════════════════════════════════════════════════════
  // Pass 1: Shadow map — render scene depth from light's perspective
  // ════════════════════════════════════════════════════════════════════════
  m_gpuProfiler->beginPass(cmd, GpuPass::Shadow, m_currentFrame);
  {
    VkClearValue depthClear{};
    depthClear.depthStencil.depth = 1.0f;
    depthClear.depthStencil.stencil = 0;

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = m_shadowMap->getRenderPass();
    rpInfo.framebuffer = m_shadowMap->getFramebuffer();
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = {ShadowMap::SHADOW_MAP_SIZE,
                                ShadowMap::SHADOW_MAP_SIZE};
    rpInfo.clearValueCount = 1;
    rpInfo.pClearValues = &depthClear;

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_shadowMap->getPipeline());

    VkViewport vp{};
    vp.width = static_cast<float>(ShadowMap::SHADOW_MAP_SIZE);
    vp.height = static_cast<float>(ShadowMap::SHADOW_MAP_SIZE);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = {ShadowMap::SHADOW_MAP_SIZE, ShadowMap::SHADOW_MAP_SIZE};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkDescriptorSet shadowDescSet = m_shadowMap->getDescSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_shadowMap->getPipelineLayout(), 0, 1,
                            &shadowDescSet, 0, nullptr);

    // Build light-space frustum for shadow culling
    Frustum lightFrustum;
    lightFrustum.update(lightVP);

    auto renderView =
        m_scene.getRegistry().view<TransformComponent, MeshComponent>();
    for (auto entity : renderView) {
      auto &transform = renderView.get<TransformComponent>(entity);
      auto &meshComp = renderView.get<MeshComponent>(entity);

      // Light-frustum culling: skip entities outside shadow camera view
      glm::mat4 model = transform.getModelMatrix();
      AABB worldAABB = meshComp.localAABB.transformed(model);
      if (!lightFrustum.isVisible(worldAABB))
        continue;

      vkCmdPushConstants(cmd, m_shadowMap->getPipelineLayout(),
                         VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4),
                         &model);
      m_scene.getMesh(meshComp.meshIndex).draw(cmd);
    }

    // Shadow pass for DynamicMesh entities (legacy CPU-skinned)
    auto dynShadowView =
        m_scene.getRegistry().view<TransformComponent, DynamicMeshComponent>();
    for (auto entity : dynShadowView) {
      auto &transform = dynShadowView.get<TransformComponent>(entity);
      auto &dynComp = dynShadowView.get<DynamicMeshComponent>(entity);
      glm::mat4 model = transform.getModelMatrix();
      vkCmdPushConstants(cmd, m_shadowMap->getPipelineLayout(),
                         VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4),
                         &model);
      auto &dynMesh = m_scene.getDynamicMesh(dynComp.dynamicMeshIndex);
      dynMesh.bind(cmd, m_currentFrame);
      dynMesh.draw(cmd);
    }

    // Shadow pass for GPU-skinned characters (uses bone SSBO in main desc set)
    if (m_shadowSkinnedPipeline != VK_NULL_HANDLE) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_shadowSkinnedPipeline);
      // Bind main descriptor set (has bone SSBO at binding 4 + UBO at 0)
      VkDescriptorSet mainDescSet = m_descriptors->getSet(m_currentFrame);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_shadowSkinnedPipelineLayout, 0, 1,
                              &mainDescSet, 0, nullptr);

      auto gpuShadowView = m_scene.getRegistry()
          .view<TransformComponent, GPUSkinnedMeshComponent, AnimationComponent>();
      uint32_t shadowSlot = 0;
      for (auto entity : gpuShadowView) {
        auto &transform = gpuShadowView.get<TransformComponent>(entity);
        auto &gpuComp   = gpuShadowView.get<GPUSkinnedMeshComponent>(entity);
        auto &animComp  = gpuShadowView.get<AnimationComponent>(entity);

        // Use pre-allocated bone slot for minions, sequential for player
        uint32_t eid = static_cast<uint32_t>(entity);
        uint32_t boneSlot = shadowSlot;
        auto bsIt = m_entityBoneSlot.find(eid);
        if (bsIt != m_entityBoneSlot.end()) {
          boneSlot = bsIt->second;
        } else {
          ++shadowSlot;
        }

        // Write bones into per-entity slot of the ring-buffer SSBO
        uint32_t boneBase = m_descriptors->writeBoneSlot(m_currentFrame, boneSlot,
                                                          animComp.player.getSkinningMatrices());

        // Push model matrix + boneBaseIndex
        glm::mat4 model = transform.getModelMatrix();
        struct ShadowPC { glm::mat4 model; uint32_t boneBase; } spc;
        spc.model    = model;
        spc.boneBase = boneBase;
        vkCmdPushConstants(cmd, m_shadowSkinnedPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(spc), &spc);
        auto &ssMesh = m_scene.getStaticSkinnedMesh(gpuComp.staticSkinnedMeshIndex);
        ssMesh.bind(cmd);
        ssMesh.draw(cmd);
      }
    }

    vkCmdEndRenderPass(cmd);
  }
  m_gpuProfiler->endPass(cmd, GpuPass::Shadow, m_currentFrame);

  // Barrier: shadow map depth write → fragment shader read
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_shadowMap->getDepthImage();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
  }

  // ════════════════════════════════════════════════════════════════════════
  // Compute pass: GPU particle simulation (skip in MOBA mode)
  // ════════════════════════════════════════════════════════════════════════
  if (!m_mobaMode) {
    m_particles->dispatchCompute(cmd, deltaTime);
  }

  // ════════════════════════════════════════════════════════════════════════
  // Compute pass: GPU frustum culling (fills indirect draw buffer)
  // Build CullObject list from all MeshComponent entities, then dispatch.
  // ════════════════════════════════════════════════════════════════════════
  {
    GLORY_PROFILE_SCOPE("GPUCull");
    std::vector<CullObject> cullObjects;
    glm::mat4 cullVP = m_mobaMode
        ? (m_isoCam.getProjectionMatrix(aspect) * m_isoCam.getViewMatrix())
        : (projMat * viewMat);

    auto cullView = m_scene.getRegistry().view<TransformComponent, MeshComponent>();
    cullObjects.reserve(cullView.size_hint());
    for (auto entity : cullView) {
      auto &tc = cullView.get<TransformComponent>(entity);
      auto &mc = cullView.get<MeshComponent>(entity);
      glm::mat4 model = tc.getModelMatrix();
      AABB worldAABB = mc.localAABB.transformed(model);

      // Resolve tint from optional ColorComponent
      glm::vec4 tint(1.0f);
      if (auto* cc = m_scene.getRegistry().try_get<ColorComponent>(entity))
        tint = cc->tint;

      // Resolve material params
      uint32_t texDiffuse = 0, texNormal = 0;
      float shininess = 32.0f, metallic = 0.0f;
      if (auto* mat = m_scene.getRegistry().try_get<MaterialComponent>(entity)) {
        texDiffuse = mat->materialIndex;
        texNormal  = mat->normalMapIndex;
        shininess  = mat->shininess;
        metallic   = mat->metallic;
      }

      const auto& mesh = m_scene.getMesh(mc.meshIndex);
      for (uint32_t mi = 0; mi < mesh.getMeshCount(); ++mi) {
        CullObject co{};
        co.aabbMin       = worldAABB.min;
        co.aabbMax       = worldAABB.max;
        co.indexCount    = mesh.getMeshIndexCount(mi);
        co.instanceCount = 1;
        co.firstIndex    = 0;
        co.vertexOffset  = 0;
        co.firstInstance = 0;
        co.modelMatrix   = model;
        co.tintAndFlags  = tint;
        co.texDiffuse    = texDiffuse;
        co.texNormal     = texNormal;
        co.shininess     = shininess;
        co.metallic      = metallic;
        cullObjects.push_back(co);
      }
    }

    m_gpuCuller.setObjects(cullObjects);
    m_gpuCuller.dispatch(cmd, cullVP, m_currentFrame);
  }

  // ════════════════════════════════════════════════════════════════════════
  // Compute pass: Compute skinning (when > threshold skinned entities)
  // Pre-skins all visible skinned characters into device-local output buffers.
  // The scene draw pass uses these pre-skinned VBOs instead of the bone SSBO path.
  // ════════════════════════════════════════════════════════════════════════
  {
    GLORY_PROFILE_SCOPE("ComputeSkin");
    auto gpuSkinnedView = m_scene.getRegistry()
        .view<TransformComponent, GPUSkinnedMeshComponent, AnimationComponent>();
    uint32_t skinnedCount = static_cast<uint32_t>(gpuSkinnedView.size_hint());

    m_activeSkinBatches.clear();
    if (skinnedCount > COMPUTE_SKIN_THRESHOLD && m_computeSkinner.isInitialised()) {
      uint32_t slot = 0;
      for (auto entity : gpuSkinnedView) {
        if (slot >= static_cast<uint32_t>(m_computeSkinEntries.size())) break;
        auto& entry   = m_computeSkinEntries[slot];
        auto& gpuComp = gpuSkinnedView.get<GPUSkinnedMeshComponent>(entity);
        auto& animComp = gpuSkinnedView.get<AnimationComponent>(entity);

        if (entry.outputBuffer.getBuffer() == VK_NULL_HANDLE) { ++slot; continue; }

        // Select LOD mesh if SkinnedLODComponent is present
        uint32_t meshIdx = gpuComp.staticSkinnedMeshIndex;
        auto *slod = m_scene.getRegistry().try_get<SkinnedLODComponent>(entity);
        if (slod && slod->levelCount > 0) {
          auto& tc  = gpuSkinnedView.get<TransformComponent>(entity);
          glm::vec3 camPos = m_mobaMode ? m_isoCam.getPosition() : m_camera.getPosition();
          float dist = glm::length(tc.position - camPos);
          for (uint32_t li = 0; li < slod->levelCount; ++li) {
            if (dist <= slod->levels[li].maxDistance) {
              meshIdx = slod->levels[li].staticSkinnedMeshIndex;
              break;
            }
          }
        }

        const auto& ssMesh = m_scene.getStaticSkinnedMesh(meshIdx);
        uint32_t boneBase  = m_descriptors->writeBoneSlot(
            m_currentFrame, slot, animComp.player.getSkinningMatrices());

        SkinBatch batch{};
        batch.bindPoseBuffer = ssMesh.getVertexBuffer();
        batch.outputBuffer   = entry.outputBuffer.getBuffer();
        batch.boneBuffer     = m_descriptors->getBoneBuffer(m_currentFrame);
        batch.vertexCount    = ssMesh.getVertexCount();
        batch.boneBaseIndex  = boneBase;
        batch.entityIndex    = slot;
        m_activeSkinBatches.push_back(batch);
        ++slot;
      }

      if (!m_activeSkinBatches.empty()) {
        m_computeSkinner.dispatch(cmd, m_activeSkinBatches, m_currentFrame);
        m_computeSkinner.insertOutputBarrier(cmd, m_activeSkinBatches);
      }
    }
  }

  m_gpuProfiler->beginPass(cmd, GpuPass::Scene, m_currentFrame);
  {
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.01f, 0.01f, 0.02f, 1.0f}};
    clearValues[1].depthStencil.depth = 1.0f;
    clearValues[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = m_postProcess->getHDRRenderPass();
    rpInfo.framebuffer = m_postProcess->getHDRFramebuffer();
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = extent;
    rpInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    rpInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Draw sky gradient (fullscreen triangle, no depth)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    // ── Terrain rendering (MOBA mode) ───────────────────────────────
    if (m_mobaMode && m_terrain) {
      // Use isometric camera for terrain view
      double mx, my;
      if (glfwGetWindowAttrib(m_window.getHandle(), GLFW_FOCUSED)) {
        glfwGetCursorPos(m_window.getHandle(), &mx, &my);
      } else {
        mx = extent.width / 2.0;
        my = extent.height / 2.0;
      }
      bool middleMouse =
          glfwGetMouseButton(m_window.getHandle(), GLFW_MOUSE_BUTTON_MIDDLE) ==
          GLFW_PRESS;
      // Get scroll delta accumulated since last frame
      float scrollDelta = m_input->consumeScrollDelta();
      m_isoCam.update(deltaTime, static_cast<float>(extent.width),
                      static_cast<float>(extent.height), mx, my, middleMouse,
                      scrollDelta);

      glm::mat4 isoView = m_isoCam.getViewMatrix();
      glm::mat4 isoProj = m_isoCam.getProjectionMatrix(aspect);
      Frustum isoFrustum;
      isoFrustum.update(isoProj * isoView);

      // Render procedural terrain only if no custom map geometry is active
      if (!m_glbMapLoaded && !m_customMap) {
        m_terrain->render(cmd, isoView, isoProj, m_isoCam.getPosition(),
                          isoFrustum, static_cast<float>(glfwGetTime()),
                          m_wireframe);
      }

      // Render map lines on top of terrain
      if (!m_customMap) drawMapDebugLines();

      // Selection circle under targeted minion
      if (m_playerEntity != entt::null &&
          m_scene.getRegistry().all_of<PlayerTargetComponent>(m_playerEntity)) {
        auto &playerTarget =
            m_scene.getRegistry().get<PlayerTargetComponent>(m_playerEntity);
        if (playerTarget.targetEntity != entt::null &&
            m_scene.getRegistry().valid(playerTarget.targetEntity) &&
            m_scene.getRegistry().all_of<TransformComponent>(
                playerTarget.targetEntity)) {
          auto &tgtTransform = m_scene.getRegistry().get<TransformComponent>(
              playerTarget.targetEntity);
          float radius = 0.6f;
          if (m_scene.getRegistry().all_of<TargetableComponent>(
                  playerTarget.targetEntity)) {
            radius = m_scene.getRegistry()
                         .get<TargetableComponent>(playerTarget.targetEntity)
                         .hitRadius;
          }
          m_debugRenderer.drawCircle(tgtTransform.position, radius,
                                     glm::vec4(1.0f, 0.1f, 0.1f, 0.8f));
        }
      }

      // Click indicator — animated textured atlas quad at right-click position
      if (m_clickIndicator) {
        // t goes from 0 (just spawned) to 1 (about to disappear)
        float t = 1.0f - (m_clickIndicator->lifetime / m_clickIndicator->maxLife);
        glm::vec4 tint = m_clickIndicator->isAttack ? glm::vec4(1.0f, 0.2f, 0.2f, 1.0f) : glm::vec4(0.2f, 1.0f, 0.2f, 1.0f);
        m_clickIndicatorRenderer->render(cmd, isoProj * isoView, m_clickIndicator->position, t, 1.5f, tint);
      }

      // Green selection circles under marquee-selected entities
      {
        auto selView = m_scene.getRegistry().view<SelectedComponent, TransformComponent>();
        for (auto entity : selView) {
          auto &selTransform = selView.get<TransformComponent>(entity);
          float selRadius = 0.6f;
          auto *tgt = m_scene.getRegistry().try_get<TargetableComponent>(entity);
          if (tgt) selRadius = tgt->hitRadius;
          m_debugRenderer.drawCircle(selTransform.position, selRadius,
                                     glm::vec4(0.2f, 1.0f, 0.3f, 0.7f));
        }
      }

      m_debugRenderer.render(cmd, isoProj * isoView);

      // ── Render scene entities in MOBA mode (character, etc.) ──────────
      {
        VkPipeline scenePipeline = m_wireframe
                                       ? m_pipeline->getWireframePipeline()
                                       : m_pipeline->getGraphicsPipeline();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline);

        // Ensure viewport & scissor are set (dynamic state)
        VkViewport vp{};
        vp.width    = static_cast<float>(extent.width);
        vp.height   = static_cast<float>(extent.height);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = extent;
        vkCmdSetScissor(cmd, 0, 1, &sc);

        // Light data for MOBA scene entities
        LightUBO lightUBOData{};
        {
          std::vector<std::pair<glm::vec3, glm::vec3>> sceneLights;
          m_scene.getAllLights(sceneLights);
          lightUBOData.lightCount = static_cast<int>(
              std::min(sceneLights.size(), static_cast<size_t>(MAX_LIGHTS)));
          for (int li = 0; li < lightUBOData.lightCount; ++li) {
            lightUBOData.lights[li].position = sceneLights[li].first;
            lightUBOData.lights[li].color = sceneLights[li].second;
          }
          lightUBOData.viewPos = m_isoCam.getPosition();
          lightUBOData.ambientStrength = 0.55f;
          lightUBOData.specularStrength = 0.5f;
          lightUBOData.shininess = 32.0f;
          lightUBOData.fogColor = glm::vec3(0.6f, 0.65f, 0.75f);
          lightUBOData.fogDensity = 0.0f;
          lightUBOData.fogStart = 5.0f;
          lightUBOData.fogEnd = 500.0f;
        }
        m_descriptors->updateLightBuffer(m_currentFrame, lightUBOData);

        UniformBufferObject ubo{};
        ubo.view = isoView;
        ubo.proj = isoProj;
        ubo.lightSpaceMatrix = lightVP;
        m_descriptors->updateUniformBuffer(m_currentFrame, ubo);

        VkDescriptorSet descSet = m_descriptors->getSet(m_currentFrame);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipeline->getPipelineLayout(), 0, 1, &descSet,
                                0, nullptr);

        // Build instance data for character entities only
        auto *instancePtr =
            static_cast<InstanceData *>(m_instanceMapped[m_currentFrame]);
        auto *indirectPtr = static_cast<VkDrawIndexedIndirectCommand *>(
            m_indirectMapped[m_currentFrame]);

        uint32_t mobaInstances = 0;
        uint32_t mobaIndirectCmds = 0;
        std::vector<DrawGroup> mobaDrawGroups;

        using GroupKey = std::tuple<uint32_t, uint32_t, uint32_t>;
        std::map<GroupKey, std::vector<InstanceData>> mobaGroups;

        auto addEntityToGroups = [&](entt::entity entity) {
          auto &transform =
              m_scene.getRegistry().get<TransformComponent>(entity);
          auto &meshComp = m_scene.getRegistry().get<MeshComponent>(entity);

          glm::mat4 modelMat = transform.getModelMatrix();

          InstanceData inst{};
          inst.model = modelMat;
          inst.normalMatrix = glm::transpose(glm::inverse(modelMat));
          inst.tint = glm::vec4(1.0f);
          inst.params = glm::vec4(0.0f, 0.0f, 0.5f, 0.0f);
          inst.texIndices = glm::vec4(0.0f);

          auto *colorComp =
              m_scene.getRegistry().try_get<ColorComponent>(entity);
          if (colorComp)
            inst.tint = colorComp->tint;

          uint32_t texIdx = 0, normIdx = 0;
          auto *matComp =
              m_scene.getRegistry().try_get<MaterialComponent>(entity);
          if (matComp) {
            texIdx = matComp->materialIndex;
            normIdx = matComp->normalMapIndex;
            inst.params = glm::vec4(matComp->shininess, matComp->metallic,
                                    matComp->roughness, matComp->emissive);
          }
          inst.texIndices = glm::vec4(static_cast<float>(texIdx),
                                      static_cast<float>(normIdx), 0.0f, 0.0f);

          mobaGroups[{meshComp.meshIndex, 0, 0}].push_back(inst);
        };

        auto charView =
            m_scene.getRegistry()
                .view<TransformComponent, MeshComponent, CharacterComponent>();
        for (auto entity : charView) {
          addEntityToGroups(entity);
        }

        auto projView =
            m_scene.getRegistry()
                .view<TransformComponent, MeshComponent, ProjectileComponent>();
        for (auto entity : projView) {
          addEntityToGroups(entity);
        }

        // Map entity (GLB map mesh)
        auto mapView =
            m_scene.getRegistry()
                .view<TransformComponent, MeshComponent, MapComponent>();
        for (auto entity : mapView) {
          addEntityToGroups(entity);
        }

        // Minion entities
        auto minionView2 =
            m_scene.getRegistry()
                .view<TransformComponent, MeshComponent, MinionTag>();
        for (auto entity : minionView2) {
          // Skip dead minions
          auto *hp = m_scene.getRegistry().try_get<MinionHealthComponent>(entity);
          if (hp && hp->isDead) continue;
          auto *mState = m_scene.getRegistry().try_get<MinionStateComponent>(entity);
          if (mState && mState->state == MinionState::Dead) continue;
          addEntityToGroups(entity);
        }

        // Minion projectiles
        auto mProjView =
            m_scene.getRegistry()
                .view<TransformComponent, MeshComponent,
                      MinionProjectileComponent>();
        for (auto entity : mProjView) {
          addEntityToGroups(entity);
        }

        // Structure entities (towers, inhibitors, nexus)
        auto structView = m_scene.getRegistry().view<TransformComponent, MeshComponent, StructureIdentityComponent>();
        for (auto entity : structView) {
            auto *hp = m_scene.getRegistry().try_get<StructureHealthComponent>(entity);
            if (hp && hp->isDead) continue;
            addEntityToGroups(entity);
        }

        // Jungle monster entities
        auto monsterView3 = m_scene.getRegistry().view<TransformComponent, MeshComponent, JungleMonsterTag>();
        for (auto entity : monsterView3) {
            auto *hp = m_scene.getRegistry().try_get<MonsterHealthComponent>(entity);
            if (hp && hp->isDead) continue;
            addEntityToGroups(entity);
        }

        // Tower projectiles
        auto towerProjView = m_scene.getRegistry().view<TransformComponent, MeshComponent, TowerProjectileComponent>();
        for (auto entity : towerProjView) {
            addEntityToGroups(entity);
        }

        for (auto &[key, instances] : mobaGroups) {
          uint32_t meshIdx = std::get<0>(key);
          const auto &model = m_scene.getMesh(meshIdx);

          DrawGroup g{};
          g.meshIndex = meshIdx;
          g.instanceOffset = mobaInstances;
          g.instanceCount = static_cast<uint32_t>(instances.size());
          g.indirectOffset = mobaIndirectCmds;
          g.indirectCount = model.getMeshCount();

          std::memcpy(instancePtr + mobaInstances, instances.data(),
                      instances.size() * sizeof(InstanceData));
          mobaInstances += g.instanceCount;

          for (uint32_t mi = 0; mi < g.indirectCount; ++mi) {
            auto &icmd = indirectPtr[mobaIndirectCmds++];
            icmd.indexCount = model.getMeshIndexCount(mi);
            icmd.instanceCount = g.instanceCount;
            icmd.firstIndex = 0;
            icmd.vertexOffset = 0;
            icmd.firstInstance = g.instanceOffset;
          }

          mobaDrawGroups.push_back(g);
        }

        visibleInstances += mobaInstances;
        indirectCmdCount += mobaIndirectCmds;

        VkBuffer instanceBufs[] = {
            m_instanceBuffers[m_currentFrame].getBuffer()};
        VkDeviceSize instanceOffsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 1, 1, instanceBufs, instanceOffsets);

        VkBuffer indirectBuf = m_indirectBuffers[m_currentFrame].getBuffer();
        for (const auto &g : mobaDrawGroups) {
          VkDeviceSize indirectOffset =
              g.indirectOffset * sizeof(VkDrawIndexedIndirectCommand);
          m_scene.getMesh(g.meshIndex)
              .drawIndirect(cmd, indirectBuf, indirectOffset);
          ++drawCalls;
        }

        // ── Draw GPU-skinned characters (bone SSBO + skinned.vert) ──────
        if (m_skinnedPipeline != VK_NULL_HANDLE) {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_skinnedPipeline);
          // Re-bind descriptor set with the skinned pipeline layout (has push constant range)
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_skinnedPipelineLayout, 0, 1, &descSet,
                                  0, nullptr);

          // Camera position for LOD distance calculation
          glm::vec3 camPos = m_mobaMode
              ? m_isoCam.getPosition()
              : m_camera.getPosition();

          auto gpuView = m_scene.getRegistry()
              .view<TransformComponent, GPUSkinnedMeshComponent,
                    AnimationComponent>();
          uint32_t sceneSlot = 0;
          for (auto entity : gpuView) {
            auto &transform = m_scene.getRegistry().get<TransformComponent>(entity);
            auto &gpuComp   = m_scene.getRegistry().get<GPUSkinnedMeshComponent>(entity);
            auto &animComp  = m_scene.getRegistry().get<AnimationComponent>(entity);

            // LOD selection: check SkinnedLODComponent if present
            uint32_t meshIdx = gpuComp.staticSkinnedMeshIndex;
            auto *slod = m_scene.getRegistry().try_get<SkinnedLODComponent>(entity);
            if (slod && slod->levelCount > 0) {
              float dist = glm::length(transform.position - camPos);
              for (uint32_t li = 0; li < slod->levelCount; ++li) {
                if (dist <= slod->levels[li].maxDistance) {
                  meshIdx = slod->levels[li].staticSkinnedMeshIndex;
                  break;
                }
              }
              // If beyond all levels, use last level
              if (dist > slod->levels[slod->levelCount - 1].maxDistance)
                meshIdx = slod->levels[slod->levelCount - 1].staticSkinnedMeshIndex;
            }

            glm::mat4 modelMat = transform.getModelMatrix();

            // Build instance data for this character
            InstanceData inst{};
            inst.model        = modelMat;
            inst.normalMatrix = glm::transpose(glm::inverse(modelMat));
            inst.tint         = glm::vec4(1.0f);
            inst.params       = glm::vec4(0.0f, 0.0f, 0.5f, 0.0f);

            auto *colorComp = m_scene.getRegistry().try_get<ColorComponent>(entity);
            if (colorComp) inst.tint = colorComp->tint;

            uint32_t texIdx = 0, normIdx = 0;
            auto *matComp = m_scene.getRegistry().try_get<MaterialComponent>(entity);
            if (matComp) {
              texIdx = matComp->materialIndex;
              normIdx = matComp->normalMapIndex;
              inst.params = glm::vec4(matComp->shininess, matComp->metallic,
                                      matComp->roughness, matComp->emissive);
            }
            inst.texIndices = glm::vec4(static_cast<float>(texIdx),
                                        static_cast<float>(normIdx), 0.0f, 0.0f);

            // Write bones into the ring-buffer SSBO slot for this entity
            uint32_t eid = static_cast<uint32_t>(entity);
            uint32_t boneSlot = sceneSlot; // default for player (slot 0)
            auto bsIt = m_entityBoneSlot.find(eid);
            if (bsIt != m_entityBoneSlot.end()) {
              boneSlot = bsIt->second; // minion: use pre-allocated slot
            } else {
              ++sceneSlot; // player: advance the sequential counter
            }
            uint32_t boneBase = m_descriptors->writeBoneSlot(
                m_currentFrame, boneSlot, animComp.player.getSkinningMatrices());

            // Push boneBaseIndex (scene skinned pipeline has no model PC, uses instance buffer)
            vkCmdPushConstants(cmd, m_skinnedPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(uint32_t), &boneBase);

            // Write instance data and bind
            std::memcpy(instancePtr + mobaInstances, &inst, sizeof(InstanceData));

            auto &ssMesh = m_scene.getStaticSkinnedMesh(meshIdx);
            // Bind skinned vertex buffer (binding 0) and instance buffer (binding 1)
            ssMesh.bind(cmd);
            VkBuffer instBuf[] = {m_instanceBuffers[m_currentFrame].getBuffer()};
            VkDeviceSize instOff[] = {mobaInstances * sizeof(InstanceData)};
            vkCmdBindVertexBuffers(cmd, 1, 1, instBuf, instOff);

            vkCmdDrawIndexed(cmd, ssMesh.getIndexCount(), 1, 0, 0, 0);
            ++mobaInstances;
            ++drawCalls;
          }
        }

        // ── Draw legacy DynamicMesh entities (CPU-skinned, kept as fallback) ──
        auto dynView = m_scene.getRegistry()
                           .view<TransformComponent, DynamicMeshComponent>();
        if (dynView.size_hint() > 0) {
          // Re-bind scene pipeline + descriptor sets (skinned pipeline left them incompatible)
          VkPipeline scenePipeline = m_wireframe
                                         ? m_pipeline->getWireframePipeline()
                                         : m_pipeline->getGraphicsPipeline();
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline);

          VkDescriptorSet sceneDescSet = m_descriptors->getSet(m_currentFrame);
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_pipeline->getPipelineLayout(), 0, 1,
                                  &sceneDescSet, 0, nullptr);

          for (auto entity : dynView) {
          auto &transform =
              m_scene.getRegistry().get<TransformComponent>(entity);
          auto &dynComp =
              m_scene.getRegistry().get<DynamicMeshComponent>(entity);
          auto &dynMesh = m_scene.getDynamicMesh(dynComp.dynamicMeshIndex);

          glm::mat4 modelMat = transform.getModelMatrix();

          // Build a single InstanceData for this entity
          InstanceData inst{};
          inst.model = modelMat;
          inst.normalMatrix = glm::transpose(glm::inverse(modelMat));
          inst.tint = glm::vec4(1.0f);
          inst.params = glm::vec4(0.0f, 0.0f, 0.5f, 0.0f);

          auto *colorComp =
              m_scene.getRegistry().try_get<ColorComponent>(entity);
          if (colorComp)
            inst.tint = colorComp->tint;

          uint32_t texIdx = 0, normIdx = 0;
          auto *matComp =
              m_scene.getRegistry().try_get<MaterialComponent>(entity);
          if (matComp) {
            texIdx = matComp->materialIndex;
            normIdx = matComp->normalMapIndex;
            inst.params = glm::vec4(matComp->shininess, matComp->metallic,
                                    matComp->roughness, matComp->emissive);
          }
          inst.texIndices = glm::vec4(static_cast<float>(texIdx),
                                      static_cast<float>(normIdx), 0.0f, 0.0f);

          // Write single instance to instance buffer
          std::memcpy(instancePtr + mobaInstances, &inst, sizeof(InstanceData));

          dynMesh.bind(cmd, m_currentFrame);
          VkBuffer dynInstBufs[] = {
              m_instanceBuffers[m_currentFrame].getBuffer()};
          VkDeviceSize dynInstOffsets[] = {mobaInstances *
                                           sizeof(InstanceData)};
          vkCmdBindVertexBuffers(cmd, 1, 1, dynInstBufs, dynInstOffsets);

          vkCmdDrawIndexed(cmd, dynMesh.getIndexCount(), 1, 0, 0, 0);
          ++mobaInstances;
          ++drawCalls;
          }
        } // dynView size_hint
      }
    } else {

      // Switch to scene pipeline (fill or wireframe)
      VkPipeline scenePipeline = m_wireframe
                                     ? m_pipeline->getWireframePipeline()
                                     : m_pipeline->getGraphicsPipeline();
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline);

      // Prepare light data once per frame (constant across entities)
      LightUBO lightUBOData{};
      {
        std::vector<std::pair<glm::vec3, glm::vec3>> sceneLights;
        m_scene.getAllLights(sceneLights);
        lightUBOData.lightCount = static_cast<int>(
            std::min(sceneLights.size(), static_cast<size_t>(MAX_LIGHTS)));
        for (int li = 0; li < lightUBOData.lightCount; ++li) {
          lightUBOData.lights[li].position = sceneLights[li].first;
          lightUBOData.lights[li].color = sceneLights[li].second;
        }
        lightUBOData.viewPos = m_camera.getPosition();
        lightUBOData.ambientStrength = 0.15f;
        lightUBOData.specularStrength = 0.5f;
        lightUBOData.shininess = 32.0f;
        lightUBOData.fogColor = glm::vec3(0.6f, 0.65f, 0.75f);
        lightUBOData.fogDensity = m_overlay->getFogDensity();
        lightUBOData.fogStart = 5.0f;
        lightUBOData.fogEnd = 50.0f;
      }
      m_descriptors->updateLightBuffer(m_currentFrame, lightUBOData);

      // Update shared UBO once per frame (view/proj/lightSpace only)
      UniformBufferObject ubo{};
      ubo.view = viewMat;
      ubo.proj = projMat;
      ubo.lightSpaceMatrix = lightVP;
      m_descriptors->updateUniformBuffer(m_currentFrame, ubo);

      VkDescriptorSet descSet = m_descriptors->getSet(m_currentFrame);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_pipeline->getPipelineLayout(), 0, 1, &descSet,
                              0, nullptr);

      // ── Multithreaded entity processing: frustum cull, LOD, instance data ──
      using GroupKey =
          std::tuple<uint32_t, uint32_t, uint32_t>; // mesh, tex, norm
      glm::vec3 camPos = m_camera.getPosition();

      // Collect entity handles (safe single-threaded snapshot)
      auto renderView =
          m_scene.getRegistry().view<TransformComponent, MeshComponent>();
      std::vector<entt::entity> entities;
      entities.reserve(renderView.size_hint());
      for (auto e : renderView)
        entities.push_back(e);

      // Per-worker result: local groups + culled count
      struct WorkerResult {
        std::map<GroupKey, std::vector<InstanceData>> groups;
        uint32_t culled = 0;
      };

      uint32_t entityCount = static_cast<uint32_t>(entities.size());
      uint32_t workerCount = std::min(m_workerCount, std::max(1u, entityCount));
      uint32_t perWorker = (entityCount + workerCount - 1) / workerCount;

      // Launch worker threads for parallel entity processing
      std::vector<std::future<WorkerResult>> futures;
      futures.reserve(workerCount);
      auto &registry = m_scene.getRegistry();

      for (uint32_t w = 0; w < workerCount; ++w) {
        uint32_t start = w * perWorker;
        uint32_t end = std::min(start + perWorker, entityCount);
        if (start >= entityCount)
          break;

        futures.push_back(
            std::async(std::launch::async, [&, start, end]() -> WorkerResult {
              WorkerResult result;
              for (uint32_t i = start; i < end; ++i) {
                auto entity = entities[i];
                auto &transform = registry.get<TransformComponent>(entity);
                auto &meshComp = registry.get<MeshComponent>(entity);

                // Frustum culling
                glm::mat4 modelMat = transform.getModelMatrix();
                AABB worldAABB = meshComp.localAABB.transformed(modelMat);
                if (!frustum.isVisible(worldAABB)) {
                  ++result.culled;
                  continue;
                }

                // LOD selection
                uint32_t selectedMesh = meshComp.meshIndex;
                auto *lodComp = registry.try_get<LODComponent>(entity);
                if (lodComp && lodComp->levelCount > 0) {
                  float dist = glm::distance(camPos, transform.position);
                  selectedMesh =
                      lodComp->levels[lodComp->levelCount - 1].meshIndex;
                  for (uint32_t li = 0; li < lodComp->levelCount; ++li) {
                    if (dist <= lodComp->levels[li].maxDistance) {
                      selectedMesh = lodComp->levels[li].meshIndex;
                      break;
                    }
                  }
                }

                // Build instance data
                InstanceData inst{};
                inst.model = modelMat;
                inst.normalMatrix = glm::transpose(glm::inverse(modelMat));
                inst.tint = glm::vec4(1.0f);
                inst.params = glm::vec4(0.0f, 0.0f, 0.5f, 0.0f);
                inst.texIndices = glm::vec4(0.0f); // default texture 0

                auto *colorComp = registry.try_get<ColorComponent>(entity);
                if (colorComp)
                  inst.tint = colorComp->tint;

                uint32_t texIdx = 0, normIdx = 0;
                auto *matComp = registry.try_get<MaterialComponent>(entity);
                if (matComp) {
                  texIdx = matComp->materialIndex;
                  normIdx = matComp->normalMapIndex;
                  inst.params =
                      glm::vec4(matComp->shininess, matComp->metallic,
                                matComp->roughness, matComp->emissive);
                }
                inst.texIndices =
                    glm::vec4(static_cast<float>(texIdx),
                              static_cast<float>(normIdx), 0.0f, 0.0f);

                // Group by mesh only — textures are now per-instance via
                // bindless
                result.groups[{selectedMesh, 0, 0}].push_back(inst);
              }
              return result;
            }));
      }

      // Merge worker results
      std::map<GroupKey, std::vector<InstanceData>> groups;
      for (auto &f : futures) {
        auto result = f.get();
        culled += result.culled;
        for (auto &[key, instances] : result.groups) {
          auto &target = groups[key];
          target.insert(target.end(),
                        std::make_move_iterator(instances.begin()),
                        std::make_move_iterator(instances.end()));
        }
      }

      // Fill instance buffer, indirect buffer, and build draw groups
      auto *instancePtr =
          static_cast<InstanceData *>(m_instanceMapped[m_currentFrame]);
      auto *indirectPtr = static_cast<VkDrawIndexedIndirectCommand *>(
          m_indirectMapped[m_currentFrame]);
      std::vector<DrawGroup> drawGroups;
      uint32_t totalInstances = 0;
      uint32_t totalIndirectCmds = 0;

      for (auto &[key, instances] : groups) {
        uint32_t meshIdx = std::get<0>(key);
        const auto &model = m_scene.getMesh(meshIdx);

        DrawGroup g{};
        g.meshIndex = meshIdx;
        g.textureIndex = std::get<1>(key);
        g.normalMapIndex = std::get<2>(key);
        g.instanceOffset = totalInstances;
        g.instanceCount = static_cast<uint32_t>(instances.size());
        g.indirectOffset = totalIndirectCmds;
        g.indirectCount = model.getMeshCount();

        std::memcpy(instancePtr + totalInstances, instances.data(),
                    instances.size() * sizeof(InstanceData));
        totalInstances += g.instanceCount;

        // Fill one VkDrawIndexedIndirectCommand per mesh in the model
        for (uint32_t mi = 0; mi < g.indirectCount; ++mi) {
          auto &icmd = indirectPtr[totalIndirectCmds++];
          icmd.indexCount = model.getMeshIndexCount(mi);
          icmd.instanceCount = g.instanceCount;
          icmd.firstIndex = 0;
          icmd.vertexOffset = 0;
          icmd.firstInstance = g.instanceOffset;
        }

        drawGroups.push_back(g);
      }
      visibleInstances = totalInstances;
      indirectCmdCount = totalIndirectCmds;

      // Bind instance buffer to binding slot 1
      VkBuffer instanceBufs[] = {m_instanceBuffers[m_currentFrame].getBuffer()};
      VkDeviceSize instanceOffsets[] = {0};
      vkCmdBindVertexBuffers(cmd, 1, 1, instanceBufs, instanceOffsets);

      // Issue indirect draw calls — no per-group descriptor updates needed
      // (bindless textures selected per-instance via texIndices in vertex data)
      VkBuffer indirectBuf = m_indirectBuffers[m_currentFrame].getBuffer();
      for (const auto &g : drawGroups) {
        VkDeviceSize indirectOffset =
            g.indirectOffset * sizeof(VkDrawIndexedIndirectCommand);
        m_scene.getMesh(g.meshIndex)
            .drawIndirect(cmd, indirectBuf, indirectOffset);
        ++drawCalls;
      }
    }

    // Debug grid (alpha-blended, depth-tested, after scene)
    if (m_showGrid) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      vkCmdSetScissor(cmd, 0, 1, &scissor);

      struct GridPC {
        glm::mat4 viewProj;
        float gridY;
      } gridPC;
      gridPC.viewProj = projMat * viewMat;
      gridPC.gridY = -1.5f; // floor level
      vkCmdPushConstants(cmd, m_gridPipelineLayout,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(gridPC), &gridPC);
      vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    // Particles (additive-blended embers, after scene geometry)
    if (!m_mobaMode) {
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      m_particles->record(cmd, projMat * viewMat);
    }

    vkCmdEndRenderPass(cmd);
  }
  m_gpuProfiler->endPass(cmd, GpuPass::Scene, m_currentFrame);

  // ════════════════════════════════════════════════════════════════════════: depth → ambient occlusion (half-res, 2-pass: compute + blur)
  // Skip in MOBA mode — minimal visual impact for top-down view
  // ════════════════════════════════════════════════════════════════════════
  if (!m_mobaMode) {
    // Barrier: ensure HDR depth writes are visible before SSAO reads
    VkImageMemoryBarrier depthBarrier{};
    depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.image = m_postProcess->getHDRDepthImage();
    depthBarrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &depthBarrier);

    glm::mat4 invProjMat = glm::inverse(projMat);
    m_ssao->record(cmd, projMat, invProjMat, m_overlay->getSSAORadius(),
                   m_overlay->getSSAOBias(), m_overlay->getSSAOIntensity());
  }

  // ════════════════════════════════════════════════════════════════════════
  // Bloom passes: extract bright → H-blur → V-blur (half-res)
  // Skip in MOBA mode for performance
  // ════════════════════════════════════════════════════════════════════════
  if (!m_mobaMode) {
    m_bloom->record(cmd, m_postProcess->getParams().bloomThreshold);
  }

  // ════════════════════════════════════════════════════════════════════════
  // Pass 3: Post-process — tone map HDR + bloom → swapchain
  // ════════════════════════════════════════════════════════════════════════
  m_gpuProfiler->beginPass(cmd, GpuPass::Post, m_currentFrame);
  {
    VkClearValue clearColor{};
    clearColor.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = m_postProcess->getRenderPass();
    rpInfo.framebuffer = m_postProcess->getFramebuffers()[imageIndex];
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = extent;
    rpInfo.clearValueCount = 1;
    rpInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Select pipeline variant: MOBA uses specialization-constant-compiled minimal
    // shader (tone map + FXAA only, all other effects compiled out by driver)
    VkPipeline ppPipeline = m_mobaMode
        ? m_postProcess->getMobaPipeline()
        : m_postProcess->getPipeline();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ppPipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    auto &params = m_postProcess->getParams();
    params.exposure = m_overlay->getExposure();
    params.gamma = m_overlay->getGamma();
    params.toneMapMode = static_cast<float>(m_overlay->getToneMapMode());
    params.fxaaEnabled = m_overlay->getFXAAEnabled() ? 1.0f : 0.0f;

    if (m_mobaMode) {
      // MOBA mode: minimal post-process for maximum performance
      // Only tone mapping + FXAA — skip all expensive effects
      params.bloomIntensity = 0.0f;
      params.bloomThreshold = 999.0f;
      params.vignetteStrength = 0.0f;
      params.chromaStrength = 0.0f;
      params.filmGrain = 0.0f;
      params.sharpenStrength = 0.0f;
      params.dofStrength = 0.0f;
      params.dofFocusDist = 5.0f;
      params.dofRange = 3.0f;
      params.saturation = 1.0f;
      params.colorTemp = 0.0f;
      params.outlineStrength = 0.0f;
      params.outlineThreshold = 0.1f;
      params.godRaysStrength = 0.0f;
      params.godRaysDecay = 0.97f;
      params.godRaysDensity = 0.0f;
      params.godRaysSamples = 0.0f;
      params.autoExposure = 0.0f;
      params.heatDistortion = 0.0f;
      params.ditheringStrength = 0.0f;
    } else {
      // Non-MOBA: full post-process from overlay controls
      params.bloomIntensity = m_overlay->getBloomIntensity();
      params.bloomThreshold = m_overlay->getBloomThreshold();
      params.vignetteStrength = m_overlay->getVignetteStrength();
      params.vignetteRadius = m_overlay->getVignetteRadius();
      params.chromaStrength = m_overlay->getChromaStrength();
      params.filmGrain = m_overlay->getFilmGrain();
      params.sharpenStrength = m_overlay->getSharpenStrength();
      params.dofStrength = m_overlay->getDofStrength();
      params.dofFocusDist = m_overlay->getDofFocusDist();
      params.dofRange = m_overlay->getDofRange();
      params.saturation = m_overlay->getSaturation();
      params.colorTemp = m_overlay->getColorTemp();
      params.outlineStrength = m_overlay->getOutlineStrength();
      params.outlineThreshold = m_overlay->getOutlineThreshold();
      params.godRaysStrength = m_overlay->getGodRaysStrength();
      params.godRaysDecay = m_overlay->getGodRaysDecay();
      params.godRaysDensity = m_overlay->getGodRaysDensity();
      params.godRaysSamples = 64.0f;
      params.autoExposure = m_overlay->getAutoExposure() ? 1.0f : 0.0f;
      params.heatDistortion = m_overlay->getHeatDistortion();
      params.ditheringStrength = m_overlay->getDitheringStrength();
    }

    // Project main light position to screen UV for god rays
    {
      glm::vec3 lightPos(2.0f, 4.0f, 2.0f);
      glm::vec3 lightColor;
      m_scene.getFirstLight(lightPos, lightColor);
      glm::vec4 clip = projMat * viewMat * glm::vec4(lightPos, 1.0f);
      if (clip.w > 0.0f) {
        glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
        params.lightScreenX = ndc.x * 0.5f + 0.5f;
        params.lightScreenY = ndc.y * 0.5f + 0.5f;
      }
    }

    vkCmdPushConstants(cmd, m_postProcess->getPipelineLayout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(PostProcessParams), &params);

    VkDescriptorSet ppDescSet = m_postProcess->getDescSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_postProcess->getPipelineLayout(), 0, 1,
                            &ppDescSet, 0, nullptr);

    vkCmdDraw(cmd, 3, 1, 0, 0); // fullscreen triangle

    // ImGui overlay (rendered in same pass, after tone mapping)
    m_gpuProfiler->beginPass(cmd, GpuPass::ImGui, m_currentFrame);
    m_overlay->render(cmd);
    m_gpuProfiler->endPass(cmd, GpuPass::ImGui, m_currentFrame);

    vkCmdEndRenderPass(cmd);
  }
  m_gpuProfiler->endPass(cmd, GpuPass::Post, m_currentFrame);

  m_overlay->setDrawCallCount(drawCalls);
  m_overlay->setCulledCount(culled);
  m_overlay->setTotalInstances(visibleInstances);
  m_overlay->setIndirectCommands(indirectCmdCount);

  // Resolve GPU timestamps into readback buffer (read after next fence wait)
  m_gpuProfiler->resolve(cmd, m_currentFrame);

  VK_CHECK(vkEndCommandBuffer(cmd), "Failed to record command buffer");
}

// ── Sky gradient pipeline ───────────────────────────────────────────────────
void Renderer::createSkyPipeline() {
  VkDevice dev = m_device->getDevice();

  auto readFile = [](const std::string &path) -> std::vector<char> {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
      throw std::runtime_error("Failed to open shader: " + path);
    size_t sz = static_cast<size_t>(file.tellg());
    std::vector<char> buf(sz);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
  };

  auto makeModule = [&](const std::vector<char> &code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t *>(code.data());
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod), "shader module");
    return mod;
  };

  auto vertCode = readFile(std::string(SHADER_DIR) + "postprocess.vert.spv");
  auto fragCode = readFile(std::string(SHADER_DIR) + "sky.frag.spv");
  VkShaderModule vertMod = makeModule(vertCode);
  VkShaderModule fragMod = makeModule(fragCode);

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertMod;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragMod;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynState{};
  dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynState.dynamicStateCount = 2;
  dynState.pDynamicStates = dynStates;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_NONE;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Depth test/write disabled — sky is always behind everything
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_FALSE;
  depthStencil.depthWriteEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  VkPipelineLayoutCreateInfo layoutCI{};
  layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

  VK_CHECK(
      vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_skyPipelineLayout),
      "Failed to create sky pipeline layout");

  VkGraphicsPipelineCreateInfo pipelineCI{};
  pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCI.stageCount = 2;
  pipelineCI.pStages = stages;
  pipelineCI.pVertexInputState = &vertexInput;
  pipelineCI.pInputAssemblyState = &inputAssembly;
  pipelineCI.pViewportState = &viewportState;
  pipelineCI.pRasterizationState = &rasterizer;
  pipelineCI.pMultisampleState = &multisample;
  pipelineCI.pDepthStencilState = &depthStencil;
  pipelineCI.pColorBlendState = &colorBlend;
  pipelineCI.pDynamicState = &dynState;
  pipelineCI.layout = m_skyPipelineLayout;
  pipelineCI.renderPass = m_postProcess->getHDRRenderPass();
  pipelineCI.subpass = 0;

  VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineCI,
                                     nullptr, &m_skyPipeline),
           "Failed to create sky pipeline");

  vkDestroyShaderModule(dev, fragMod, nullptr);
  vkDestroyShaderModule(dev, vertMod, nullptr);
  spdlog::info("Sky gradient pipeline created");
}

void Renderer::destroySkyPipeline() {
  VkDevice dev = m_device->getDevice();
  if (m_skyPipeline)
    vkDestroyPipeline(dev, m_skyPipeline, nullptr);
  if (m_skyPipelineLayout)
    vkDestroyPipelineLayout(dev, m_skyPipelineLayout, nullptr);
  m_skyPipeline = VK_NULL_HANDLE;
  m_skyPipelineLayout = VK_NULL_HANDLE;
}

// ── Debug grid pipeline ─────────────────────────────────────────────────────
void Renderer::createGridPipeline() {
  VkDevice dev = m_device->getDevice();

  auto readFile = [](const std::string &path) -> std::vector<char> {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
      throw std::runtime_error("Failed to open shader: " + path);
    size_t sz = static_cast<size_t>(file.tellg());
    std::vector<char> buf(sz);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
  };

  auto makeModule = [&](const std::vector<char> &code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t *>(code.data());
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod), "shader module");
    return mod;
  };

  auto vertCode = readFile(std::string(SHADER_DIR) + "grid.vert.spv");
  auto fragCode = readFile(std::string(SHADER_DIR) + "grid.frag.spv");
  VkShaderModule vertMod = makeModule(vertCode);
  VkShaderModule fragMod = makeModule(fragCode);

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertMod;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragMod;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynState{};
  dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynState.dynamicStateCount = 2;
  dynState.pDynamicStates = dynStates;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = VK_CULL_MODE_NONE;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // Depth test enabled (grid behind objects), depth write disabled
  // (transparent)
  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_FALSE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

  // Alpha blending for transparent grid lines
  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = VK_TRUE;
  blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments = &blendAttachment;

  // Push constant: mat4 viewProj + float gridY = 68 bytes
  VkPushConstantRange pushRange{};
  pushRange.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pushRange.offset = 0;
  pushRange.size = sizeof(glm::mat4) + sizeof(float);

  VkPipelineLayoutCreateInfo layoutCI{};
  layoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCI.pushConstantRangeCount = 1;
  layoutCI.pPushConstantRanges = &pushRange;

  VK_CHECK(
      vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_gridPipelineLayout),
      "Failed to create grid pipeline layout");

  VkGraphicsPipelineCreateInfo pipelineCI{};
  pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCI.stageCount = 2;
  pipelineCI.pStages = stages;
  pipelineCI.pVertexInputState = &vertexInput;
  pipelineCI.pInputAssemblyState = &inputAssembly;
  pipelineCI.pViewportState = &viewportState;
  pipelineCI.pRasterizationState = &rasterizer;
  pipelineCI.pMultisampleState = &multisample;
  pipelineCI.pDepthStencilState = &depthStencil;
  pipelineCI.pColorBlendState = &colorBlend;
  pipelineCI.pDynamicState = &dynState;
  pipelineCI.layout = m_gridPipelineLayout;
  pipelineCI.renderPass = m_postProcess->getHDRRenderPass();
  pipelineCI.subpass = 0;

  VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineCI,
                                     nullptr, &m_gridPipeline),
           "Failed to create grid pipeline");

  vkDestroyShaderModule(dev, fragMod, nullptr);
  vkDestroyShaderModule(dev, vertMod, nullptr);
  spdlog::info("Debug grid pipeline created");
}

void Renderer::destroyGridPipeline() {
  VkDevice dev = m_device->getDevice();
  if (m_gridPipeline)
    vkDestroyPipeline(dev, m_gridPipeline, nullptr);
  if (m_gridPipelineLayout)
    vkDestroyPipelineLayout(dev, m_gridPipelineLayout, nullptr);
  m_gridPipeline = VK_NULL_HANDLE;
  m_gridPipelineLayout = VK_NULL_HANDLE;
}

// ── Instance buffer management ──────────────────────────────────────────────
void Renderer::createInstanceBuffers() {
  m_instanceCapacity = INITIAL_INSTANCE_CAPACITY;
  VkDeviceSize bufSize = sizeof(InstanceData) * m_instanceCapacity;

  m_instanceBuffers.reserve(Sync::MAX_FRAMES_IN_FLIGHT);
  m_instanceMapped.resize(Sync::MAX_FRAMES_IN_FLIGHT);

  for (uint32_t i = 0; i < Sync::MAX_FRAMES_IN_FLIGHT; ++i) {
    m_instanceBuffers.emplace_back(m_device->getAllocator(), bufSize,
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                   VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_instanceMapped[i] = m_instanceBuffers[i].map();
  }

  spdlog::info("Instance buffers created ({} capacity, {} bytes each)",
               m_instanceCapacity, bufSize);
}

void Renderer::destroyInstanceBuffers() {
  for (auto &buf : m_instanceBuffers) {
    buf.unmap();
  }
  m_instanceBuffers.clear();
  m_instanceMapped.clear();
}

// ── Indirect draw buffers ────────────────────────────────────────────────────
void Renderer::createIndirectBuffers() {
  m_indirectCapacity =
      INITIAL_INSTANCE_CAPACITY; // one command per draw group max
  VkDeviceSize bufSize =
      sizeof(VkDrawIndexedIndirectCommand) * m_indirectCapacity;

  m_indirectBuffers.reserve(Sync::MAX_FRAMES_IN_FLIGHT);
  m_indirectMapped.resize(Sync::MAX_FRAMES_IN_FLIGHT);

  for (uint32_t i = 0; i < Sync::MAX_FRAMES_IN_FLIGHT; ++i) {
    m_indirectBuffers.emplace_back(m_device->getAllocator(), bufSize,
                                   VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                   VMA_MEMORY_USAGE_CPU_TO_GPU);
    m_indirectMapped[i] = m_indirectBuffers[i].map();
  }

  spdlog::info("Indirect draw buffers created ({} capacity, {} bytes each)",
               m_indirectCapacity, bufSize);
}

void Renderer::destroyIndirectBuffers() {
  for (auto &buf : m_indirectBuffers) {
    buf.unmap();
  }
  m_indirectBuffers.clear();
  m_indirectMapped.clear();
}

// ── Per-thread command pools & secondary buffers ─────────────────────────────
void Renderer::createThreadResources() {
  m_workerCount = std::min(std::thread::hardware_concurrency(), 4u);
  if (m_workerCount < 2)
    m_workerCount = 2;

  VkDevice dev = m_device->getDevice();
  auto indices = m_device->getQueueFamilies();

  m_threadCommandPools.resize(m_workerCount);
  m_secondaryBuffers.resize(Sync::MAX_FRAMES_IN_FLIGHT);

  for (uint32_t t = 0; t < m_workerCount; ++t) {
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = indices.graphicsFamily.value();
    VK_CHECK(vkCreateCommandPool(dev, &ci, nullptr, &m_threadCommandPools[t]),
             "Failed to create thread command pool");
  }

  for (uint32_t f = 0; f < Sync::MAX_FRAMES_IN_FLIGHT; ++f) {
    m_secondaryBuffers[f].resize(m_workerCount);
    for (uint32_t t = 0; t < m_workerCount; ++t) {
      VkCommandBufferAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ai.commandPool = m_threadCommandPools[t];
      ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
      ai.commandBufferCount = 1;
      VK_CHECK(vkAllocateCommandBuffers(dev, &ai, &m_secondaryBuffers[f][t]),
               "Failed to allocate secondary command buffer");
    }
  }

  spdlog::info("Thread resources created ({} workers, {} secondary buffers)",
               m_workerCount, m_workerCount * Sync::MAX_FRAMES_IN_FLIGHT);
}

void Renderer::destroyThreadResources() {
  VkDevice dev = m_device->getDevice();
  for (auto &pool : m_threadCommandPools) {
    if (pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(dev, pool, nullptr);
  }
  m_threadCommandPools.clear();
  m_secondaryBuffers.clear();
}

// ── GPU-skinned character pipeline ──────────────────────────────────────────
// Uses skinned.vert (SkinnedVertex binding 0 + bone SSBO binding 4) +
// triangle.frag (same fragment shader as regular scene pipeline).
void Renderer::createSkinnedPipeline() {
  VkDevice dev = m_device->getDevice();

  auto readFile = [](const std::string &path) -> std::vector<char> {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
      throw std::runtime_error("Failed to open shader: " + path);
    size_t sz = static_cast<size_t>(file.tellg());
    std::vector<char> buf(sz);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
  };
  auto makeModule = [&](const std::vector<char> &code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t *>(code.data());
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &mod), "shader module");
    return mod;
  };

  VkShaderModule vertMod = makeModule(readFile(std::string(SHADER_DIR) + "skinned.vert.spv"));
  VkShaderModule fragMod = makeModule(readFile(std::string(SHADER_DIR) + "triangle.frag.spv"));

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main", nullptr};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main", nullptr};

  // Vertex input: SkinnedVertex (binding 0, per-vertex) +
  //              InstanceData   (binding 1, per-instance)
  auto skinnedBinding  = SkinnedVertex::getBindingDescription();
  auto skinnedAttrs    = SkinnedVertex::getAttributeDescriptions();
  auto instanceBinding = InstanceData::getBindingDescription();
  // Instance binding uses location 6+ to avoid conflict with skinned locations 0-5
  auto instanceAttrs   = InstanceData::getAttributeDescriptions();
  // The instance InstanceData getAttributeDescriptions() starts at location 4.
  // We need to shift to 6 to be after SkinnedVertex's 0-5 range.
  for (auto &a : instanceAttrs) a.location += 2; // 4→6, 5→7, …, 14→16

  // Instance binding number should be 1
  instanceBinding.binding = 1;
  for (auto &a : instanceAttrs) a.binding = 1;

  std::vector<VkVertexInputAttributeDescription> allAttrs;
  allAttrs.insert(allAttrs.end(), skinnedAttrs.begin(), skinnedAttrs.end());
  allAttrs.insert(allAttrs.end(), instanceAttrs.begin(), instanceAttrs.end());

  std::array<VkVertexInputBindingDescription, 2> bindings = {skinnedBinding, instanceBinding};

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount   = static_cast<uint32_t>(bindings.size());
  vertexInput.pVertexBindingDescriptions      = bindings.data();
  vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(allAttrs.size());
  vertexInput.pVertexAttributeDescriptions    = allAttrs.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynState{};
  dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynState.dynamicStateCount = 2;
  dynState.pDynamicStates    = dynStates;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount  = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth   = 1.0f;
  rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
  rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable  = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

  VkPipelineColorBlendAttachmentState blendAttach{};
  blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blendAttach.blendEnable = VK_FALSE;

  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1;
  colorBlend.pAttachments    = &blendAttach;

  // Reuse the same descriptor set layout (already has binding 4 SSBO)
  // Add push constant for boneBaseIndex (uint, vertex stage only)
  VkDescriptorSetLayout descLayout = m_descriptors->getLayout();
  VkPushConstantRange skinnedPC{};
  skinnedPC.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  skinnedPC.offset     = 0;
  skinnedPC.size       = sizeof(uint32_t); // boneBaseIndex
  VkPipelineLayoutCreateInfo layoutCI{};
  layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutCI.setLayoutCount         = 1;
  layoutCI.pSetLayouts            = &descLayout;
  layoutCI.pushConstantRangeCount = 1;
  layoutCI.pPushConstantRanges    = &skinnedPC;
  VK_CHECK(vkCreatePipelineLayout(dev, &layoutCI, nullptr, &m_skinnedPipelineLayout),
           "Failed to create skinned pipeline layout");

  VkGraphicsPipelineCreateInfo pipelineCI{};
  pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineCI.stageCount          = 2;
  pipelineCI.pStages             = stages;
  pipelineCI.pVertexInputState   = &vertexInput;
  pipelineCI.pInputAssemblyState = &inputAssembly;
  pipelineCI.pViewportState      = &viewportState;
  pipelineCI.pRasterizationState = &rasterizer;
  pipelineCI.pMultisampleState   = &multisample;
  pipelineCI.pDepthStencilState  = &depthStencil;
  pipelineCI.pColorBlendState    = &colorBlend;
  pipelineCI.pDynamicState       = &dynState;
  pipelineCI.layout              = m_skinnedPipelineLayout;
  pipelineCI.renderPass          = m_postProcess->getHDRRenderPass();
  pipelineCI.subpass             = 0;

  VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineCI,
                                     nullptr, &m_skinnedPipeline),
           "Failed to create skinned pipeline");

  // ── Shadow pipeline variant for GPU-skinned characters ────────────────────
  // Uses the main descriptor set (binding 0=UBO with lightSpaceMatrix,
  // binding 4=bone SSBO) and a push constant for the model matrix.
  VkShaderModule shadowVertMod = makeModule(
      readFile(std::string(SHADER_DIR) + "shadow_skinned.vert.spv"));
  VkShaderModule shadowFragMod = makeModule(
      readFile(std::string(SHADER_DIR) + "shadow.frag.spv"));

  VkPipelineShaderStageCreateInfo shadowStages[2]{};
  shadowStages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_VERTEX_BIT, shadowVertMod, "main", nullptr};
  shadowStages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_FRAGMENT_BIT, shadowFragMod, "main", nullptr};

  // Reuse the main descriptor layout (has UBO binding 0 AND bone SSBO binding 4)
  // Push constants: mat4 model (0..63) + uint boneBaseIndex (64..67)
  VkPushConstantRange shadowPCRange{};
  shadowPCRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  shadowPCRange.offset     = 0;
  shadowPCRange.size       = sizeof(glm::mat4) + sizeof(uint32_t); // model + boneBaseIndex

  VkPipelineLayoutCreateInfo shadowLayoutCI{};
  shadowLayoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  shadowLayoutCI.setLayoutCount         = 1;
  shadowLayoutCI.pSetLayouts            = &descLayout;  // main layout has binding 0+4
  shadowLayoutCI.pushConstantRangeCount = 1;
  shadowLayoutCI.pPushConstantRanges    = &shadowPCRange;

  VK_CHECK(vkCreatePipelineLayout(dev, &shadowLayoutCI, nullptr, &m_shadowSkinnedPipelineLayout),
           "Failed to create shadow-skinned pipeline layout");

  // Shadow depth/stencil
  VkPipelineDepthStencilStateCreateInfo shadowDepth = depthStencil;
  shadowDepth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

  // No colour blend for shadow pass
  VkPipelineColorBlendStateCreateInfo shadowColorBlend{};
  shadowColorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  shadowColorBlend.attachmentCount = 0;

  VkPipelineRasterizationStateCreateInfo shadowRaster = rasterizer;
  shadowRaster.depthBiasEnable         = VK_TRUE;
  shadowRaster.depthBiasConstantFactor = 1.25f;
  shadowRaster.depthBiasSlopeFactor    = 1.75f;
  shadowRaster.cullMode                = VK_CULL_MODE_FRONT_BIT;

  VkGraphicsPipelineCreateInfo shadowPipelineCI = pipelineCI;
  shadowPipelineCI.stageCount          = 2;
  shadowPipelineCI.pStages             = shadowStages;
  shadowPipelineCI.pRasterizationState = &shadowRaster;
  shadowPipelineCI.pDepthStencilState  = &shadowDepth;
  shadowPipelineCI.pColorBlendState    = &shadowColorBlend;
  shadowPipelineCI.layout              = m_shadowSkinnedPipelineLayout;
  shadowPipelineCI.renderPass          = m_shadowMap->getRenderPass();

  VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &shadowPipelineCI,
                                     nullptr, &m_shadowSkinnedPipeline),
           "Failed to create shadow-skinned pipeline");

  vkDestroyShaderModule(dev, shadowVertMod, nullptr);
  vkDestroyShaderModule(dev, shadowFragMod, nullptr);
  vkDestroyShaderModule(dev, vertMod, nullptr);
  vkDestroyShaderModule(dev, fragMod, nullptr);

  spdlog::info("GPU-skinned scene + shadow pipelines created");
}

void Renderer::destroySkinnedPipeline() {
  VkDevice dev = m_device->getDevice();
  if (m_skinnedPipeline != VK_NULL_HANDLE)
    vkDestroyPipeline(dev, m_skinnedPipeline, nullptr);
  if (m_shadowSkinnedPipeline != VK_NULL_HANDLE)
    vkDestroyPipeline(dev, m_shadowSkinnedPipeline, nullptr);
  if (m_skinnedPipelineLayout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(dev, m_skinnedPipelineLayout, nullptr);
  if (m_shadowSkinnedPipelineLayout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(dev, m_shadowSkinnedPipelineLayout, nullptr);
  m_skinnedPipeline              = VK_NULL_HANDLE;
  m_shadowSkinnedPipeline        = VK_NULL_HANDLE;
  m_skinnedPipelineLayout        = VK_NULL_HANDLE;
  m_shadowSkinnedPipelineLayout  = VK_NULL_HANDLE;
}

} // namespace glory
