#include "renderer/Renderer.h"
#include "renderer/Frustum.h"
#include "renderer/VkCheck.h"
#include "window/Window.h"

#include "ability/AbilityComponents.h"
#include "ability/AbilityDef.h"
#include "ability/AbilitySystem.h"
#include "ability/VFXEventQueue.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
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
  m_bloom = std::make_unique<Bloom>(*m_device, *m_swapchain,
                                    m_postProcess->getHDRImageView());
  m_ssao = std::make_unique<SSAO>(*m_device, *m_swapchain,
                                  m_postProcess->getHDRDepthView());
  m_postProcess->updateBloomDescriptor(m_bloom->getOutputImageView());
  m_postProcess->updateSSAODescriptor(m_ssao->getOutputImageView());
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
  m_overlay = std::make_unique<DebugOverlay>();
  m_overlay->init(m_window.getHandle(), m_context->getInstance(), *m_device,
                  *m_swapchain, m_postProcess->getRenderPass());
  buildScene();
  createSkyPipeline();
  createGridPipeline();
  createInstanceBuffers();
  createIndirectBuffers();
  createThreadResources();
  // Terrain system
  m_terrain = std::make_unique<TerrainSystem>();
  m_terrain->init(*m_device, m_postProcess->getHDRRenderPass());
  m_isoCam.setBounds(glm::vec3(0, 0, 0), glm::vec3(200, 0, 200));
  m_scene.setTerrainSystem(m_terrain.get());
  m_input = std::make_unique<InputManager>(m_window.getHandle(), m_camera);
  m_input->setCaptureEnabled(!m_mobaMode);

  try {
    m_mapData = MapLoader::LoadFromFile(std::string(MAP_DATA_DIR) +
                                        "map_summonersrift.json");
    spdlog::info("Loaded map data");
  } catch (const std::exception &e) {
    spdlog::warn("Failed to load map data: {}", e.what());
  }

  m_debugRenderer.init(*m_device, m_postProcess->getHDRRenderPass());

  m_lastFrameTime = static_cast<float>(glfwGetTime());
  spdlog::info("Renderer initialized");
}

Renderer::~Renderer() {
  m_input.reset();
  m_overlay.reset();
  m_sync.reset();
  destroySkyPipeline();
  destroyGridPipeline();
  destroyThreadResources();
  destroyIndirectBuffers();
  destroyInstanceBuffers();
  m_terrain.reset();
  m_particles.reset();
  m_pipeline.reset();
  m_descriptors.reset();
  m_scene = Scene{};
  m_shadowMap.reset();
  m_bloom.reset();
  m_ssao.reset();
  m_postProcess.reset();
  m_swapchain.reset();
  m_device.reset();
  m_window.destroySurface(m_context->getInstance());
  m_context.reset();
  m_debugRenderer.cleanup();
  spdlog::info("Renderer destroyed");
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

  m_input->update(deltaTime);
  m_scene.update(deltaTime);
  m_projectileSystem.update(m_scene, deltaTime);
  if (m_particles) { // Guard particle rendering
    m_particles->update(deltaTime);
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

        // Set target on character entities
        auto charView = m_scene.getRegistry().view<CharacterComponent>();
        for (auto entity : charView) {
          auto &character = charView.get<CharacterComponent>(entity);
          character.targetPosition = hitPos;
          character.hasTarget = true;
        }
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

  m_overlay->setWireframe(m_wireframe);

  // Feed debug overlay stats
  m_overlay->setFPS(deltaTime > 0.0f ? 1.0f / deltaTime : 0.0f);
  m_overlay->setFrameTime(deltaTime * 1000.0f);
  auto pos = m_camera.getPosition();
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
  m_overlay->endFrame();

  // 1. Wait for this frame slot's previous work to finish
  VkFence fence = m_sync->getInFlightFence(m_currentFrame);
  vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

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

  // 3. Record command buffer
  VkCommandBuffer cmd = m_sync->getCommandBuffer(m_currentFrame);
  vkResetCommandBuffer(cmd, 0);
  recordCommandBuffer(cmd, imageIndex, deltaTime);

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

  // LOD mesh variants (lower-detail versions of base meshes)
  uint32_t sphereLOD1 = m_scene.addMesh(Model::createSphere(
      *m_device, m_device->getAllocator(), 16, 32)); // medium
  uint32_t sphereLOD2 = m_scene.addMesh(
      Model::createSphere(*m_device, m_device->getAllocator(), 8, 16)); // low

  uint32_t defaultTex = m_scene.addTexture(Texture::createDefault(*m_device));
  uint32_t checkerTex =
      m_scene.addTexture(Texture::createCheckerboard(*m_device));
  uint32_t flatNorm = m_scene.addTexture(Texture::createFlatNormal(*m_device));
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

  // Write all scene textures into the bindless descriptor array
  uint32_t texCount = static_cast<uint32_t>(m_scene.getTextures().size());
  for (uint32_t t = 0; t < texCount; ++t) {
    auto &tex = m_scene.getTexture(t);
    m_descriptors->writeBindlessTexture(t, tex.getImageView(),
                                        tex.getSampler());
  }
  m_descriptors->updateShadowMap(m_shadowMap->getDepthView(),
                                 m_shadowMap->getSampler());

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

  // ── Player character (scientist model at map center, click-to-move) ──────
  uint32_t charMesh = 0;
  uint32_t charTex = defaultTex;
  try {
    std::string glbPath = std::string(MODEL_DIR) + "test_scientist.glb";
    charMesh = m_scene.addMesh(
        Model::loadFromGLB(*m_device, m_device->getAllocator(), glbPath));
    auto glbTextures = Model::loadGLBTextures(*m_device, glbPath);
    if (!glbTextures.empty()) {
      charTex = m_scene.addTexture(std::move(glbTextures[0]));
    }
    spdlog::info("Loaded scientist model with {} texture(s)",
                 glbTextures.size());
  } catch (const std::exception &e) {
    spdlog::error("Failed to load scientist GLB: {} – falling back to capsule",
                  e.what());
    charMesh = m_scene.addMesh(
        Model::createCapsule(*m_device, m_device->getAllocator()));
  }
  auto character = m_scene.createEntity("PlayerCharacter");
  m_scene.getRegistry().emplace<MeshComponent>(character,
                                               MeshComponent{charMesh});
  m_scene.getRegistry().emplace<MaterialComponent>(
      character, MaterialComponent{charTex, flatNorm, 0.0f, 0.3f, 0.4f});
  m_scene.getRegistry().emplace<ColorComponent>(
      character, ColorComponent{glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)});
  m_scene.getRegistry().emplace<CharacterComponent>(character);
  auto &charT = m_scene.getRegistry().get<TransformComponent>(character);
  charT.position = glm::vec3(100.0f, 0.0f, 100.0f);
  charT.scale = glm::vec3(0.05f);

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
      db.loadFromFile("assets/abilities/fire_mage_fireball.json");
      db.loadFromFile("assets/abilities/fire_mage_flame_pillar.json");
      db.loadFromFile("assets/abilities/fire_mage_molten_shield.json");
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

  spdlog::info(
      "Scene built: spheres + cubes, 3 lights, orbiter, sky gradient + bloom");
}

// ── Light view-projection matrix ────────────────────────────────────────────
glm::mat4 Renderer::computeLightVP() const {
  glm::vec3 lightPos(2.0f, 4.0f, 2.0f);
  glm::vec3 lightColor;
  m_scene.getFirstLight(lightPos, lightColor);

  glm::mat4 lightView =
      glm::lookAt(lightPos, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  glm::mat4 lightProj = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 30.0f);
  lightProj[1][1] *= -1.0f; // Vulkan Y-flip
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
  m_bloom->recreate(*m_swapchain, m_postProcess->getHDRImageView());
  m_ssao->recreate(*m_swapchain, m_postProcess->getHDRDepthView());
  m_postProcess->updateBloomDescriptor(m_bloom->getOutputImageView());
  m_postProcess->updateSSAODescriptor(m_ssao->getOutputImageView());
  m_postProcess->updateDepthDescriptor(m_postProcess->getHDRDepthView());
  m_pipeline->recreateFramebuffers(
      *m_swapchain); // no-op for external render pass
  m_sync->recreateRenderFinishedSemaphores(m_swapchain->getImageCount());
  m_overlay->onSwapchainRecreate();

  spdlog::info("Swapchain recreation complete");
}

// ── Command recording — 3-pass pipeline ─────────────────────────────────────
void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex,
                                   float deltaTime) {
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo),
           "Failed to begin recording command buffer");

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
  // Pass 1: Shadow map — render scene depth from light's perspective
  // ════════════════════════════════════════════════════════════════════════
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

    auto renderView =
        m_scene.getRegistry().view<TransformComponent, MeshComponent>();
    for (auto entity : renderView) {
      auto &transform = renderView.get<TransformComponent>(entity);
      glm::mat4 model = transform.getModelMatrix();
      vkCmdPushConstants(cmd, m_shadowMap->getPipelineLayout(),
                         VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4),
                         &model);
      m_scene.getMesh(renderView.get<MeshComponent>(entity).meshIndex)
          .draw(cmd);
    }

    vkCmdEndRenderPass(cmd);
  }

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
  // Compute pass: GPU particle simulation
  // ════════════════════════════════════════════════════════════════════════
  m_particles->dispatchCompute(cmd, deltaTime);

  // ════════════════════════════════════════════════════════════════════════
  // Pass 2: Scene — render to HDR offscreen target
  // ════════════════════════════════════════════════════════════════════════
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
      // Get scroll from a simple poll (scroll is consumed by callback, use last
      // known)
      m_isoCam.update(deltaTime, static_cast<float>(extent.width),
                      static_cast<float>(extent.height), mx, my, middleMouse,
                      0.0f);

      glm::mat4 isoView = m_isoCam.getViewMatrix();
      glm::mat4 isoProj = m_isoCam.getProjectionMatrix(aspect);
      Frustum isoFrustum;
      isoFrustum.update(isoProj * isoView);

      m_terrain->render(cmd, isoView, isoProj, m_isoCam.getPosition(),
                        isoFrustum, static_cast<float>(glfwGetTime()),
                        m_wireframe);

      // Render map lines on top of terrain
      drawMapDebugLines();
      m_debugRenderer.render(cmd, isoProj * isoView);

      // ── Render scene entities in MOBA mode (character, etc.) ──────────
      {
        VkPipeline scenePipeline = m_wireframe
                                       ? m_pipeline->getWireframePipeline()
                                       : m_pipeline->getGraphicsPipeline();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline);

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
          lightUBOData.ambientStrength = 0.3f;
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

  // ════════════════════════════════════════════════════════════════════════
  // SSAO pass: depth → ambient occlusion (half-res, 2-pass: compute + blur)
  // ════════════════════════════════════════════════════════════════════════
  {
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
  // ════════════════════════════════════════════════════════════════════════
  m_bloom->record(cmd, m_postProcess->getParams().bloomThreshold);

  // ════════════════════════════════════════════════════════════════════════
  // Pass 3: Post-process — tone map HDR + bloom → swapchain
  // ════════════════════════════════════════════════════════════════════════
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

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_postProcess->getPipeline());

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
    params.bloomIntensity = m_overlay->getBloomIntensity();
    params.bloomThreshold = m_overlay->getBloomThreshold();
    params.vignetteStrength = m_overlay->getVignetteStrength();
    params.vignetteRadius = m_overlay->getVignetteRadius();
    params.chromaStrength = m_overlay->getChromaStrength();
    params.filmGrain = m_overlay->getFilmGrain();
    params.toneMapMode = static_cast<float>(m_overlay->getToneMapMode());
    params.fxaaEnabled = m_overlay->getFXAAEnabled() ? 1.0f : 0.0f;
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
    m_overlay->render(cmd);

    vkCmdEndRenderPass(cmd);
  }

  m_overlay->setDrawCallCount(drawCalls);
  m_overlay->setCulledCount(culled);
  m_overlay->setTotalInstances(visibleInstances);
  m_overlay->setIndirectCommands(indirectCmdCount);

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

} // namespace glory
