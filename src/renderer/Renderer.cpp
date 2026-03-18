#include "renderer/Renderer.h"
#include "renderer/FogOfWarRenderer.h"
#include "renderer/VkCheck.h"
#include "renderer/Model.h"
#include "renderer/StaticSkinnedMesh.h"
#include "renderer/Frustum.h"
#include "renderer/passes/PassSetup.h"
#include "core/SimulationLoop.h"
#include "core/Profiler.h"
#include "scene/Components.h"
#include "combat/CombatComponents.h"
#include "combat/HeroDefinition.h"
#include "combat/MinionWaveSystem.h"
#include "combat/RespawnSystem.h"
#include "ability/AbilityComponents.h"
#include "scripting/LuaBindings.h"
#include "map/MapLoader.h"
#include "nav/NavMeshBuilder.h"
#include "window/Window.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>

namespace glory {

// ── Constructor ──────────────────────────────────────────────────────────────
Renderer::Renderer(Window& window) : m_window(window) {
    m_context = std::make_unique<Context>();
    m_window.createSurface(m_context->getInstance());
    m_device    = std::make_unique<Device>(m_context->getInstance(), m_window.getSurface());
    m_swapchain = std::make_unique<Swapchain>(*m_device, m_window.getSurface(), m_window.getExtent());

    m_hdrFB = std::make_unique<HDRFramebuffer>();
    m_hdrFB->init(*m_device, m_window.getExtent().width, m_window.getExtent().height,
                  VK_FORMAT_R16G16B16A16_SFLOAT,
                  m_device->findSupportedFormat(
                      {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                      VK_IMAGE_TILING_OPTIMAL,
                      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));

    m_descriptors = std::make_unique<Descriptors>(*m_device, Sync::MAX_FRAMES_IN_FLIGHT);
    m_bindless    = std::make_unique<BindlessDescriptors>(*m_device);
    m_pipeline = std::make_unique<Pipeline>(*m_device, *m_swapchain,
                                            m_descriptors->getLayout(),
                                            m_bindless->getLayout(),
                                            m_hdrFB->mainFormats());
    m_sync = std::make_unique<Sync>(*m_device, m_swapchain->getImageCount());

    // Dummy 1×1 white texture bound to the shadow-map descriptor slot.
    // The shader's calcShadow() reads depth 1.0 from it → always returns 1.0 (no shadow).
    m_dummyShadow = Texture::createDefault(*m_device);

    // Initialize cascaded shadow maps — replaces dummy shadow with real atlas
    m_shadowPass.init(*m_device, m_descriptors->getLayout());
    m_shadowPass.bindToDescriptors(*m_descriptors);

    m_clickIndicatorRenderer = std::make_unique<ClickIndicatorRenderer>(
        *m_device, m_hdrFB->mainFormats());
    m_groundDecalRenderer = std::make_unique<GroundDecalRenderer>(
        *m_device, m_hdrFB->mainFormats());
    // Register basic AoE decal
    GroundDecalRenderer::DecalDef circleDecal;
    circleDecal.id = "decal_circle";
    circleDecal.color = {1.0f, 1.0f, 1.0f, 0.5f};
    circleDecal.duration = 2.0f;
    m_groundDecalRenderer->registerDecal(circleDecal);

    // Targeting indicator decals — long duration, destroyed manually
    GroundDecalRenderer::DecalDef skillshotDecal;
    skillshotDecal.id = "ability_skillshot";
    skillshotDecal.color = {0.3f, 0.6f, 1.0f, 0.4f};
    skillshotDecal.duration = 60.0f;
    skillshotDecal.fadeInTime = 0.0f;
    skillshotDecal.fadeOutTime = 0.0f;
    m_groundDecalRenderer->registerDecal(skillshotDecal);

    GroundDecalRenderer::DecalDef aoeDecal;
    aoeDecal.id = "ability_aoe";
    aoeDecal.color = {1.0f, 0.3f, 0.3f, 0.35f};
    aoeDecal.duration = 60.0f;
    aoeDecal.fadeInTime = 0.0f;
    aoeDecal.fadeOutTime = 0.0f;
    m_groundDecalRenderer->registerDecal(aoeDecal);

    GroundDecalRenderer::DecalDef rangeDecal;
    rangeDecal.id = "ability_range";
    rangeDecal.color = {1.0f, 1.0f, 1.0f, 0.15f};
    rangeDecal.duration = 60.0f;
    rangeDecal.fadeInTime = 0.0f;
    rangeDecal.fadeOutTime = 0.0f;
    m_groundDecalRenderer->registerDecal(rangeDecal);

    m_distortionRenderer = std::make_unique<DistortionRenderer>(
        *m_device, m_hdrFB->loadFormats(), m_hdrFB->colorCopyView(), m_hdrFB->sampler());
    // Register basic ripple distortion
    DistortionDef ripple;
    ripple.id = "vfx_ripple";
    ripple.duration = 1.0f;
    ripple.radius = 8.0f;
    ripple.strength = 1.0f;
    m_distortionRenderer->registerDef(ripple);

    m_inkingPass = std::make_unique<InkingPass>();
    m_inkingPass->init(*m_device, m_hdrFB->loadFormats(),
                       m_hdrFB->characterDepthView(), m_hdrFB->sampler());

    m_fogOfWar = std::make_unique<FogOfWarRenderer>();
    m_fogOfWar->init(*m_device);

    m_shieldBubble = std::make_unique<ShieldBubbleRenderer>();
    m_shieldBubble->init(*m_device, m_hdrFB->mainFormats());
    m_coneEffect = std::make_unique<ConeAbilityRenderer>();
    m_coneEffect->init(*m_device, m_hdrFB->mainFormats());
    m_explosionRenderer = std::make_unique<ExplosionRenderer>();
    m_explosionRenderer->init(*m_device, m_hdrFB->mainFormats());

    // Sprite-atlas-based VFX renderer
    m_spriteEffectRenderer = std::make_unique<SpriteEffectRenderer>();
    m_spriteEffectRenderer->init(*m_device, m_hdrFB->mainFormats());
    try {
        m_spriteEffectConeW = m_spriteEffectRenderer->registerEffect(
            "cone_w", std::string(ASSET_DIR) + "textures/cone_w_atlas.png",
            8, 56, 3.5f, true);
    } catch (const std::exception& e) {
        spdlog::warn("Sprite atlas cone_w not found: {} — effect disabled", e.what());
    }
    try {
        m_spriteEffectExplosionE = m_spriteEffectRenderer->registerEffect(
            "explosion_e", std::string(ASSET_DIR) + "textures/explosion_e_atlas.png",
            8, 56, 4.2f, true);
    } catch (const std::exception& e) {
        spdlog::warn("Sprite atlas explosion_e not found: {} — effect disabled", e.what());
    }

    m_debugRenderer.init(*m_device, m_hdrFB->mainFormats());

    createInstanceBuffers();
    createGridPipeline();
    createSkinnedPipeline();

    m_outlineRenderer = std::make_unique<OutlineRenderer>();
    m_outlineRenderer->init(*m_device, m_hdrFB->mainFormats(),
                            m_descriptors->getLayout());
    m_vfxQueue      = std::make_unique<VFXEventQueue>();
    m_combatVfxQueue = std::make_unique<VFXEventQueue>(); // separate SPSC for CombatSystem
    m_vfxRenderer   = std::make_unique<VFXRenderer>(*m_device, m_hdrFB->mainFormats());
    m_vfxRenderer->setDepthBuffer(m_hdrFB->depthView(), m_hdrFB->sampler());
    m_vfxRenderer->loadEmitterDirectory(std::string(ASSET_DIR) + "vfx/");

    // Data-driven VFX definition registry (new-style JSON with multi-force, shapes)
    m_vfxDefLoader.loadDirectory(std::string(ASSET_DIR) + "vfx/");
    m_vfxFactory.init(&m_vfxDefLoader, m_vfxRenderer.get());

    // Async compute for particle simulation
    m_asyncCompute.init(*m_device);

    // Per-thread command pools for multi-threaded secondary CB recording
    m_cmdPools.init(m_device->getDevice(),
                    m_device->getQueueFamilies().graphicsFamily.value(),
                    m_threadPool.workerCount());

    m_trailRenderer = std::make_unique<TrailRenderer>(*m_device, m_hdrFB->mainFormats());

    // Original fireball trail (kept for backward compatibility with existing IDs)
    TrailDef fireballTrail;
    fireballTrail.id         = "vfx_fireball_trail";
    fireballTrail.widthStart = 0.6f;
    fireballTrail.widthEnd   = 0.1f;
    fireballTrail.colorStart = {1.0f, 0.6f, 0.2f, 1.0f};
    fireballTrail.colorEnd   = {1.0f, 0.2f, 0.0f, 0.0f};
    fireballTrail.fadeSpeed  = 4.0f;
    fireballTrail.emitInterval = 0.01f;
    m_trailRenderer->registerTrail(fireballTrail);

    // LoL-palette fireball trail: white-gold core → orange-red → fade
    // Matches assets/trails/fireball_trail.json
    TrailDef fireballTrailLoL;
    fireballTrailLoL.id          = "fireball_trail";
    fireballTrailLoL.widthStart  = 0.6f;
    fireballTrailLoL.widthEnd    = 0.05f;
    fireballTrailLoL.colorStart  = {1.0f, 0.9f, 0.5f, 0.9f};
    fireballTrailLoL.colorEnd    = {0.8f, 0.2f, 0.0f, 0.0f};
    fireballTrailLoL.fadeSpeed   = 4.0f;              // 1/0.25s lifetime
    fireballTrailLoL.emitInterval = 0.015f;            // ~segmentDistance/speed
    fireballTrailLoL.additive    = true;
    m_trailRenderer->registerTrail(fireballTrailLoL);

    m_meshEffectRenderer = std::make_unique<MeshEffectRenderer>(*m_device, m_hdrFB->mainFormats());
    // Register basic slash effect
    MeshEffectDef slashDef;
    slashDef.id = "vfx_slash";
    slashDef.meshPath = std::string(MODEL_DIR) + "models/abiliities_models/q_bolt.glb"; // fallback mesh
    slashDef.vertShader = "mesh_effect.vert.spv";
    slashDef.fragShader = "mesh_effect_slash.frag.spv";
    slashDef.duration = 0.4f;
    slashDef.scaleStart = 0.5f;
    slashDef.scaleEnd = 2.0f;
    slashDef.colorStart = {1.0f, 0.8f, 0.4f, 1.0f};
    slashDef.colorEnd = {1.0f, 0.4f, 0.0f, 0.0f};
    m_meshEffectRenderer->registerDef(slashDef);

    m_abilitySystem = std::make_unique<AbilitySystem>(*m_vfxQueue);
    m_abilitySystem->loadDirectory(std::string(ASSET_DIR) + "abilities/");
    m_abilitySystem->getSequencer().loadDirectory(std::string(ASSET_DIR) + "vfx/composites/");

    // ── Lua scripting VM ─────────────────────────────────────────────────
    m_scriptEngine.init();
    registerAllBindings(m_scriptEngine, m_scene.getRegistry(),
                        m_abilitySystem.get(), m_vfxQueue.get(), &m_gameTime);
    m_abilitySystem->setScriptEngine(&m_scriptEngine);

    m_projectileSystem = std::make_unique<ProjectileSystem>();
    m_combatSystem  = std::make_unique<CombatSystem>(*m_combatVfxQueue);
    m_economySystem = std::make_unique<EconomySystem>();
    m_structureSystem = std::make_unique<StructureSystem>();
    m_minionWaveSystem = std::make_unique<MinionWaveSystem>();
    m_respawnSystem = std::make_unique<RespawnSystem>();

    // Wire economy system into combat and projectile systems
    m_combatSystem->setEconomySystem(m_economySystem.get());
    m_projectileSystem->setEconomySystem(m_economySystem.get());

    // ── Audio engine ─────────────────────────────────────────────────────
    m_audioEngine.init();
    m_audioResources = std::make_unique<AudioResourceManager>(m_audioEngine);
    m_audioResources->loadDirectory(std::string(ASSET_DIR) + "audio/", SoundGroup::SFX);
    m_audioEvents = std::make_unique<GameAudioEvents>(*m_audioResources);
    m_audioEvents->loadConfig(std::string(ASSET_DIR) + "config/audio_events.json");
    m_combatSystem->setAudioEvents(m_audioEvents.get());
    m_abilitySystem->setAudioEvents(m_audioEvents.get());

    m_gpuCollision.init(*m_device);

    // ── Staging pool for CPU→GPU uploads ──────────────────────────────────
    m_stagingPool.init(m_device->getAllocator());

    m_bloom = std::make_unique<BloomPass>();
    m_bloom->init(*m_device, m_hdrFB->colorView(), m_hdrFB->sampler(), m_window.getExtent().width, m_window.getExtent().height);

    RenderFormats swapFmts = RenderFormats::swapchain(m_swapchain->getImageFormat());

    m_toneMap = std::make_unique<ToneMapPass>();
    m_toneMap->init(*m_device, swapFmts, m_hdrFB->colorView(), m_bloom->bloomResultView(), m_hdrFB->sampler());

    // ── GPU-driven indirect rendering infrastructure ──
    {
        m_megaBuffer = std::make_unique<MegaBuffer>();
        m_megaBuffer->init(*m_device,
                           /*vertexCapacity=*/2 * 1024 * 1024,   // 2M vertices (~88 MB)
                           /*indexCapacity=*/4 * 1024 * 1024);    // 4M indices (~16 MB)

        auto ext = m_window.getExtent();
        m_hizPass.init(*m_device, ext.width, ext.height);
        m_gpuCuller.init(*m_device, MAX_INSTANCES);

        // Per-frame scene buffer SSBO
        VkDeviceSize sceneBufSize = MAX_INSTANCES * sizeof(GpuObjectData);
        m_sceneBuffers.reserve(Sync::MAX_FRAMES_IN_FLIGHT);
        m_sceneMapped.reserve(Sync::MAX_FRAMES_IN_FLIGHT);
        for (uint32_t i = 0; i < Sync::MAX_FRAMES_IN_FLIGHT; ++i) {
            m_sceneBuffers.emplace_back(
                m_device->getAllocator(), sceneBufSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_MEMORY_USAGE_CPU_TO_GPU);
            m_sceneMapped.push_back(m_sceneBuffers.back().map());
        }
        spdlog::info("GPU-driven rendering infrastructure initialized");
    }

    // ── Post-processing: SSAO / SSR (must be after HiZ init for pyramid view) ─
    {
        auto ext = m_window.getExtent();
        m_ssaoPass.init(*m_device, ext.width, ext.height,
                        m_hdrFB->depthView(), m_hdrFB->sampler());
        m_ssrPass.init(*m_device, ext.width, ext.height,
                       m_hdrFB->depthView(), m_hdrFB->sampler(),
                       m_hdrFB->colorView(), m_hdrFB->sampler(),
                       m_hizPass.getPyramidView(), m_hizPass.getSampler());
    }

    m_isoCam.setBounds(glm::vec3(0, 0, 0), glm::vec3(200, 0, 200));
    m_isoCam.setTarget(glm::vec3(100, 0, 100));

    // ── InputManager — MUST be created before ImGui so ImGui chains on top ──
    m_input = std::make_unique<InputManager>(m_window.getHandle(), m_camera);
    m_input->setCaptureEnabled(false); // MOBA mode: IsometricCamera drives view

    // ── ImGui initialisation ──────────────────────────────────────────────
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        // install_callbacks=false: we forward events manually from InputManager
        ImGui_ImplGlfw_InitForVulkan(m_window.getHandle(), false);

        // Dedicated descriptor pool for ImGui
        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }
        };
        VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolCI.maxSets       = 1;
        poolCI.poolSizeCount = 1;
        poolCI.pPoolSizes    = poolSizes;
        VK_CHECK(vkCreateDescriptorPool(m_device->getDevice(), &poolCI, nullptr, &m_imguiPool),
                 "Create ImGui descriptor pool");

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.Instance       = m_context->getInstance();
        initInfo.PhysicalDevice = m_device->getPhysicalDevice();
        initInfo.Device         = m_device->getDevice();
        initInfo.QueueFamily    = m_device->getQueueFamilies().graphicsFamily.value();
        initInfo.Queue          = m_device->getGraphicsQueue();
        initInfo.DescriptorPool = m_imguiPool;
        initInfo.MinImageCount  = 2;
        initInfo.ImageCount     = m_swapchain->getImageCount();
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        initInfo.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        VkFormat swapFormat = m_swapchain->getImageFormat();
        initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapFormat;
        ImGui_ImplVulkan_Init(&initInfo);
        // ImGui 1.91.8 auto-uploads fonts on first render — no one-shot buffer needed
    }

    // ── GPU timer for per-pass profiling ──────────────────────────────────
    m_gpuTimer = std::make_unique<GpuTimer>(
        m_device->getDevice(), m_device->getPhysicalDevice(),
        Sync::MAX_FRAMES_IN_FLIGHT, /*maxZones=*/32);

    m_lastFrameTime = static_cast<float>(glfwGetTime());

    // ── Build the render graph ───────────────────────────────────────────
    buildDefaultRenderGraph(m_renderGraph);

    // Wire complex passes to Renderer methods
    if (auto* p = m_renderGraph.findPass("Shadow")) {
        p->execute = [this](VkCommandBuffer cmd, const FrameContext& ctx) {
            recordShadowPass(cmd, ctx);
        };
    }
    if (auto* p = m_renderGraph.findPass("GBuffer")) {
        p->execute = [this](VkCommandBuffer cmd, const FrameContext& ctx) {
            recordGBufferPass(cmd, ctx);
        };
    }
    if (auto* p = m_renderGraph.findPass("TransparentVFX")) {
        p->execute = [this](VkCommandBuffer cmd, const FrameContext& ctx) {
            recordTransparentVFXPass(cmd, ctx);
        };
    }
    if (auto* p = m_renderGraph.findPass("Tonemap")) {
        p->execute = [this](VkCommandBuffer cmd, const FrameContext& ctx) {
            recordTonemapPass(cmd, ctx);
        };
    }

    // Re-compile after wiring callbacks (order doesn't change, but ensures consistency)
    m_renderGraph.compile();

    // ── Hero definitions ────────────────────────────────────────────────
    m_heroRegistry.loadFromDirectory(std::string(ASSET_DIR) + "heroes");

    spdlog::info("Renderer initialized");
}

// ── Destructor ───────────────────────────────────────────────────────────────
Renderer::~Renderer() {
    if (m_device) vkDeviceWaitIdle(m_device->getDevice());

    // ── ImGui shutdown ────────────────────────────────────────────────────
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (m_imguiPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(m_device->getDevice(), m_imguiPool, nullptr);

    m_gpuTimer.reset();

    m_stagingPool.destroy();

    m_combatSystem.reset();
    m_gpuCollision.destroy();
    m_input.reset();
    m_clickIndicatorRenderer.reset();
    m_groundDecalRenderer.reset();
    m_distortionRenderer.reset();
    m_inkingPass.reset();
    m_fogOfWar.reset();
    if (m_waterRenderer) { m_waterRenderer->destroy(); m_waterRenderer.reset(); }
    m_shieldBubble.reset();
    m_coneEffect.reset();
    m_explosionRenderer.reset();
    m_spriteEffectRenderer.reset();
    m_vfxRenderer.reset();
    m_asyncCompute.destroy();
    m_cmdPools.destroy();
    m_trailRenderer.reset();
    m_meshEffectRenderer.reset();
    m_vfxQueue.reset();

    m_combatVfxQueue.reset();
    m_abilitySystem.reset();

    if (m_toneMap) m_toneMap->destroy();
    m_toneMap.reset();
    m_ssaoPass.destroy();
    m_ssrPass.destroy();
    if (m_bloom) m_bloom->destroy();
    m_bloom.reset();
    if (m_hdrFB) m_hdrFB->destroy();
    m_hdrFB.reset();

    m_debugRenderer.cleanup();
    destroyInstanceBuffers();

    // GPU-driven indirect rendering cleanup
    m_gpuCuller.destroy();
    m_hizPass.destroy();
    for (auto& buf : m_sceneBuffers) buf.destroy();
    m_sceneBuffers.clear();
    m_sceneMapped.clear();
    if (m_megaBuffer) { m_megaBuffer->destroy(); m_megaBuffer.reset(); }

    destroyGridPipeline();
    destroySkinnedPipeline();
    m_dummyShadow = Texture{};
    m_toonRamp    = Texture{};
    m_impostorSystem.cleanup();
    m_shadowPass.destroy();
    m_descriptors.reset();
    m_bindless.reset();
    m_sync.reset();
    m_pipeline.reset();
    m_scene = Scene{};

    // Always destroy in this order regardless of how far construction got.
    // m_swapchain/m_device are null-safe (reset() on nullptr is a no-op).
    m_swapchain.reset();
    m_device.reset();

    // Surface must be destroyed after device but before instance.
    // We always destroy it if context exists (surface may have been created
    // before Device was successfully constructed).
    if (m_context) {
        m_window.destroySurface(m_context->getInstance());
        m_context.reset();
    }
    spdlog::info("Renderer destroyed");
}

void Renderer::waitIdle() { vkDeviceWaitIdle(m_device->getDevice()); }

// ── simulateStep ─────────────────────────────────────────────────────────────
void Renderer::simulateStep(float dt) {
    GLORY_ZONE_N("SimulateStep");
    if (m_menuMode) return;

    // Clear debug draws at start of each physics step
    m_debugRenderer.clear();

    // Save previous transforms for interpolation
    {
        auto view = m_scene.getRegistry().view<TransformComponent>();
        for (auto [e, t] : view.each()) {
            t.prevPosition = t.position;
            t.prevRotation = t.rotation;
        }
    }

    m_gameTime += dt;
    m_currentDt = dt;

    // ── Audio listener follows camera ────────────────────────────────────
    {
        glm::vec3 camPos = m_isoCam.getPosition();
        glm::vec3 camFwd = glm::normalize(m_isoCam.getTarget() - camPos);
        m_audioEngine.setListenerPosition(camPos, camFwd, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    if (m_audioResources) m_audioResources->tick();

    // ── VFX definition hot-reload (checks filesystem timestamps) ─────────
    m_vfxFactory.tickHotReload();

    // ── Lua script hot-reload ────────────────────────────────────────────
    m_scriptEngine.reloadModified();

    // ── GPU collision: read back previous frame's results ────────────────
    m_gpuCollision.readResults(m_currentFrame);

    // ── Navigation: update obstacles + regenerate dirty flow fields ──────
    m_dynamicObstacles.update(dt);
    m_pathfinding.updateFlowFields(&m_dynamicObstacles);

    // ── Simulation tick (gameplay + VFX updates, decoupled from rendering) ─
    {
        SimulationContext simCtx{
            .registry       = m_scene.getRegistry(),
            .dt             = dt,
            .abilities      = m_abilitySystem.get(),
            .projectiles    = m_projectileSystem.get(),
            .combat         = m_combatSystem.get(),
            .economy        = m_economySystem.get(),
            .structures     = m_structureSystem.get(),
            .minionWaves    = m_minionWaveSystem.get(),
            .respawn        = m_respawnSystem.get(),
            .gpuCollision   = &m_gpuCollision,
            .vfxRenderer    = m_vfxRenderer.get(),
            .vfxQueue       = m_vfxQueue.get(),
            .combatVfxQueue = m_combatVfxQueue.get(),
            .trailRenderer  = m_trailRenderer.get(),
            .groundDecals   = m_groundDecalRenderer.get(),
            .meshEffects    = m_meshEffectRenderer.get(),
            .distortion     = m_distortionRenderer.get(),
            .explosions     = m_explosionRenderer.get(),
            .coneEffect     = m_coneEffect.get(),
            .spriteEffects  = m_spriteEffectRenderer.get(),
            .coneEffectTimer = m_coneEffectTimer,
            .coneDuration    = CONE_DURATION,
            .coneHalfAngle   = CONE_HALF_ANGLE,
            .coneRange       = CONE_RANGE,
            .coneApex        = m_coneApex,
            .coneDirection   = m_coneDirection,
            .threadPool      = &m_threadPool,
            .gameTime        = &m_gameTime,
        };
        m_simLoop.tick(simCtx);
        m_coneEffectTimer = simCtx.coneEffectTimer;

        // ── Fog of War vision update ──────────────────────────────────
        if (m_fogOfWar) {
            auto& reg = m_scene.getRegistry();
            // FogOfWarGameplay gathers vision sources, updates FogSystem,
            // and manages enemy visibility states (fade-in/out/ghost).
            m_fowGameplay.update(reg, m_fogSystem, dt, Team::PLAYER);
            m_fogOfWar->updateVisibility(
                m_fogSystem.getVisibilityBuffer().data(), 128, 128);
        }
    }

    // ── Input ────────────────────────────────────────────────────────────
    auto ext = m_swapchain->getExtent();
    float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);

    int winW, winH;
    glfwGetWindowSize(m_window.getHandle(), &winW, &winH);

    double mx, my;
    glfwGetCursorPos(m_window.getHandle(), &mx, &my);

    // IsometricCamera: scroll to zoom, middle-mouse drag, edge pan
    // Per spec §5: confine cursor when focused; pass focus flag to suppress edge pan otherwise.
    const bool windowFocused = glfwGetWindowAttrib(m_window.getHandle(), GLFW_FOCUSED) != 0;
    if (windowFocused) {
        glfwSetInputMode(m_window.getHandle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
    m_isoCam.update(dt, static_cast<float>(winW), static_cast<float>(winH),
                    mx, my,
                    glfwGetMouseButton(m_window.getHandle(), GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS,
                    m_input->consumeScrollDelta(),
                    windowFocused);

    // F4 toggles debug grid
    if (m_input->wasF4Pressed()) m_showGrid = !m_showGrid;

    // Y toggles camera attach/detach (LoL-style)
    if (m_input->wasYPressed()) {
        m_isoCam.toggleAttached();
        spdlog::info("Camera {}", m_isoCam.isAttached() ? "attached" : "detached");
    }

    // Tab toggles scoreboard (hold pattern); F5 for debug UI
    if (m_input->wasTabPressed()) m_hud.scoreboard().toggle();
    // F3 toggles perf overlay
    if (m_input->wasF3Pressed()) m_perfOverlay.toggle();

    // Pick entity under cursor for highlighting and combat targeting
    m_hoveredEntity = pickEntityUnderCursor();

    // ── HUD / Minimap update ─────────────────────────────────────────────
    {
        MinimapUpdateContext hudCtx{
            m_scene.getRegistry(),
            m_playerEntity,
            m_isoCam,
            m_mapData,
            static_cast<float>(winW),
            static_cast<float>(winH),
            aspect,
            *m_input
        };
        m_hud.update(hudCtx);

        // Process minimap action requests
        if (hudCtx.actions.detachCamera)
            m_isoCam.setAttached(false);
        if (hudCtx.actions.moveCameraTo)
            m_isoCam.setTarget(hudCtx.actions.cameraTarget);
        if (hudCtx.actions.movePlayerTo && m_playerEntity != entt::null) {
            auto& reg = m_scene.getRegistry();
            auto& c   = reg.get<CharacterComponent>(m_playerEntity);
            auto& cmb = reg.get<CombatComponent>(m_playerEntity);
            cmb.targetEntity = entt::null;
            c.targetPosition = hudCtx.actions.moveTarget;
            c.hasTarget      = true;
            m_clickAnim = ClickAnim{ hudCtx.actions.moveTarget, 0.0f, 0.25f };
            if (cmb.state == CombatState::ATTACK_WINDUP || cmb.state == CombatState::ATTACK_WINDDOWN)
                cmb.state = CombatState::IDLE;
        }
    }

    // ── Gameplay update ──────────────────────────────────────────────────
    {
        GameplayInput gi;
        gi.mousePos       = m_input->getMousePos();
        gi.lastClickPos   = m_input->getLastClickPos();
        gi.rightClicked   = m_input->wasRightClicked();
        gi.leftMouseDown  = m_input->isLeftMouseDown();
        gi.shiftHeld      = glfwGetKey(m_window.getHandle(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
        gi.minimapHovered = m_hud.isMinimapHovered();
        gi.qPressed       = m_input->wasQPressed();
        gi.wPressed       = m_input->wasWPressed();
        gi.ePressed       = m_input->wasEPressed();
        gi.rPressed       = m_input->wasRPressed();
        gi.aPressed       = m_input->wasAPressed();
        gi.sPressed       = m_input->wasSPressed();
        gi.dPressed       = m_input->wasDPressed();
        gi.xKeyDown       = glfwGetKey(m_window.getHandle(), GLFW_KEY_X) == GLFW_PRESS;
        gi.leftClicked    = m_input->wasLeftClicked();
        gi.ctrlHeld       = glfwGetKey(m_window.getHandle(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS
                         || glfwGetKey(m_window.getHandle(), GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        gi.escPressed     = glfwGetKey(m_window.getHandle(), GLFW_KEY_ESCAPE) == GLFW_PRESS;
        gi.fKeyPressed    = glfwGetKey(m_window.getHandle(), GLFW_KEY_F) == GLFW_PRESS;
        gi.view           = m_isoCam.getViewMatrix();
        gi.proj           = m_isoCam.getProjectionMatrix(aspect);
        gi.screenW        = static_cast<float>(winW);
        gi.screenH        = static_cast<float>(winH);
        gi.hoveredEntity  = m_hoveredEntity;

        GameplayOutput go;
        m_gameplaySystem.update(dt, gi, go);

        if (go.clickAnim) m_clickAnim = *go.clickAnim;
        if (go.cameraFollowTarget) {
            // Don't follow player when dead — camera stays at death location
            bool isDead = RespawnSystem::isDead(m_scene.getRegistry(), m_playerEntity);
            if (!isDead)
                m_isoCam.setFollowTarget(*go.cameraFollowTarget);
        }

        // Ward placement (F key, edge-detected)
        if (gi.fKeyPressed && !m_prevFKeyDown && m_playerEntity != entt::null) {
            auto& reg = m_scene.getRegistry();
            if (reg.valid(m_playerEntity) && reg.all_of<TransformComponent>(m_playerEntity)) {
                glm::vec3 pos = reg.get<TransformComponent>(m_playerEntity).position;
                m_fowGameplay.placeWard(reg, pos, Team::PLAYER);
            }
        }
        m_prevFKeyDown = gi.fKeyPressed;
    }
}

// ── renderFrame ──────────────────────────────────────────────────────────────
void Renderer::renderFrame(float alpha) {
    GLORY_ZONE_N("RenderFrame");
    m_renderAlpha = alpha;

    // Compute real per-render-frame dt for FPS display and click anim
    float currentTimeSec = static_cast<float>(glfwGetTime());
    float realDt = std::min(currentTimeSec - m_lastFrameTime, 0.05f);
    m_lastFrameTime = currentTimeSec;

    // ImGui new frame (once per rendered frame, for both Launcher and gameplay)
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (m_menuMode && m_menuRenderer) {
        m_menuRenderer();
    }

    // ── GPU: wait + acquire swapchain image ─────────────────────────────────
    VkDevice dev = m_device->getDevice();
    m_sync->waitForFrame(m_currentFrame);

    // ── Per-frame allocator housekeeping ─────────────────────────────────────
    m_frameAllocator.reset();
    // After waitForFrame(), the previous submission for this frame slot is complete.
    // Use m_currentFrame as a simple generation counter for staging reclaim.
    m_stagingPool.reclaimCompleted(m_currentFrame);

    uint32_t imageIndex = 0;
    VkSemaphore imgSem = m_sync->getImageAvailableSemaphore(m_currentFrame);
    VkResult result = vkAcquireNextImageKHR(dev, m_swapchain->getSwapchain(),
                                             UINT64_MAX, imgSem, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("Failed to acquire swapchain image");

    // ── Resolve GPU timer from the COMPLETED frame (timeline guarantees it) ──
    if (m_gpuTimer && m_gpuTimer->isSupported()) {
        m_lastGpuResults = m_gpuTimer->resolve(m_currentFrame);
        m_lastGpuTotalMs = m_gpuTimer->totalMs(m_currentFrame);
    }

    // ── Bone SSBO write (safe after fence — GPU done with this frame's SSBO) ──
    if (!m_menuMode) {
        auto animView = m_scene.getRegistry()
            .view<SkeletonComponent, AnimationComponent, GPUSkinnedMeshComponent, TransformComponent>();
        uint32_t currentBoneSlot = 0;
        for (auto&& [e, skel, anim, ssm, t] : animView.each()) {
            const auto& matrices = anim.player.getSkinningMatrices();
            m_descriptors->writeBoneSlot(m_currentFrame, currentBoneSlot, matrices);
            ssm.boneSlot = currentBoneSlot++;
        }
        if (currentBoneSlot > 0)
            m_descriptors->flushBones(m_currentFrame);
    }

    // ── Debug UI (ImGui) ─────────────────────────────────────────────────
    if (m_showDebugUI && !m_menuMode) {
        ImGui::Begin("Debug Tools", &m_showDebugUI);

        if (ImGui::Button("Spawn Test Enemy")) {
            spawnTestEnemy();
        }
        ImGui::SameLine();
        ImGui::Text("Entities: %d", (int)m_scene.getRegistry().storage<entt::entity>().size());

        ImGui::Checkbox("Fog Enabled", &m_fogEnabled);

        ImGui::Separator();
        ImGui::Text("FPS: %.1f", realDt > 0.0f ? 1.0f / realDt : 0.0f);
        ImGui::Text("Game Time: %.1f", m_gameTime);

        if (m_playerEntity != entt::null && m_scene.getRegistry().all_of<CombatComponent>(m_playerEntity)) {
            auto& combat = m_scene.getRegistry().get<CombatComponent>(m_playerEntity);
            ImGui::Separator();
            ImGui::Text("Combat State: %s",
                combat.state == CombatState::IDLE            ? "IDLE" :
                combat.state == CombatState::ATTACK_WINDUP   ? "ATTACK_WINDUP" :
                combat.state == CombatState::ATTACK_FIRE     ? "ATTACK_FIRE" :
                combat.state == CombatState::ATTACK_WINDDOWN ? "ATTACK_WINDDOWN" :
                combat.state == CombatState::SHIELDING       ? "SHIELDING" :
                combat.state == CombatState::TRICKING        ? "TRICKING" :
                combat.state == CombatState::STUNNED         ? "STUNNED" : "???");
            ImGui::Text("Attack CD: %.2f", combat.attackCooldown);
            ImGui::Text("Shield CD: %.2f", combat.shieldCooldown);
            ImGui::Text("Trick CD:  %.2f", combat.trickCooldown);
        }

        ImGui::End();
    }

    // ── Perf overlay (F3) ─────────────────────────────────────────────────
    m_perfOverlay.draw(m_lastGpuResults, m_lastGpuTotalMs,
                       static_cast<float>(realDt * 1000.0f));

    // ── In-game HUD overlays (health bars, ability bar, floating text, etc.)
    if (!m_menuMode) {
        auto ext2 = m_swapchain->getExtent();
        float a2  = static_cast<float>(ext2.width) / static_cast<float>(ext2.height);
        glm::mat4 vp = m_isoCam.getProjectionMatrix(a2) * m_isoCam.getViewMatrix();
        int hw, hh;
        glfwGetWindowSize(m_window.getHandle(), &hw, &hh);
        uint8_t playerTeam = 0;
        if (m_playerEntity != entt::null &&
            m_scene.getRegistry().all_of<TeamComponent>(m_playerEntity))
            playerTeam = static_cast<uint8_t>(
                m_scene.getRegistry().get<TeamComponent>(m_playerEntity).team);
        m_hud.renderOverlays(m_scene.getRegistry(), m_playerEntity, vp,
                             static_cast<float>(hw), static_cast<float>(hh),
                             realDt, playerTeam);
    }

    ImGui::Render(); // always call Render() even if no windows built

    // ── Click animation ───────────────────────────────────────────────────
    if (m_clickAnim) {
        m_clickAnim->lifetime += realDt;
        if (m_clickAnim->lifetime >= m_clickAnim->maxLife)
            m_clickAnim.reset();
    }

    VkCommandBuffer cmd = m_sync->getCommandBuffer(m_currentFrame);
    vkResetCommandBuffer(cmd, 0);

    // ── Async compute: particle simulation + GPU spatial hash ────────────
    uint64_t computeSignal = 0;
    if (!m_menuMode) {
        m_asyncCompute.waitForCompute(m_currentFrame); // wait for prev frame's compute

        // Upload entity positions for GPU spatial hash
        m_gpuCollision.uploadEntities(m_scene.getRegistry(), m_currentFrame);

        VkCommandBuffer computeCmd = m_asyncCompute.begin(m_currentFrame);
        uint32_t computeFamily = m_asyncCompute.getQueueFamilyIndex();
        uint32_t graphicsFamily = m_device->getQueueFamilies().graphicsFamily.value();

        // Dispatch GPU spatial hash + broadphase collision
        m_gpuCollision.dispatch(computeCmd, m_currentFrame, computeFamily, graphicsFamily);

        // Dispatch particle simulation
        if (m_vfxRenderer) {
            m_vfxRenderer->setCameraPosition(m_isoCam.getPosition());
            m_vfxRenderer->dispatchComputeAsync(computeCmd, computeFamily, graphicsFamily);
        }

        computeSignal = m_asyncCompute.submit(m_currentFrame);
    }

    recordCommandBuffer(cmd, imageIndex, realDt);

    // ── Submit via Synchronization2 (vkQueueSubmit2 + timeline semaphore) ──
    uint64_t signalValue = m_sync->nextSignalValue(m_currentFrame);

    // Wait semaphores: image acquire + (optionally) async compute completion
    VkSemaphoreSubmitInfo waitSemInfos[2]{};
    uint32_t waitCount = 1;
    waitSemInfos[0] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitSemInfos[0].semaphore = imgSem;
    waitSemInfos[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    if (computeSignal > 0) {
        waitSemInfos[1] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waitSemInfos[1].semaphore = m_asyncCompute.getTimelineSemaphore();
        waitSemInfos[1].value     = computeSignal;
        waitSemInfos[1].stageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        waitCount = 2;
    }

    VkCommandBufferSubmitInfo cmdBufInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdBufInfo.commandBuffer = cmd;

    VkSemaphoreSubmitInfo signalSemInfos[2]{};
    // Binary semaphore for presentation engine
    signalSemInfos[0]           = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalSemInfos[0].semaphore = m_sync->getRenderFinishedSemaphore(imageIndex);
    signalSemInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    // Timeline semaphore for CPU frame synchronization
    signalSemInfos[1]           = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalSemInfos[1].semaphore = m_sync->getTimelineSemaphore();
    signalSemInfos[1].value     = signalValue;
    signalSemInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.waitSemaphoreInfoCount   = waitCount;
    submitInfo.pWaitSemaphoreInfos      = waitSemInfos;
    submitInfo.commandBufferInfoCount   = 1;
    submitInfo.pCommandBufferInfos      = &cmdBufInfo;
    submitInfo.signalSemaphoreInfoCount = 2;
    submitInfo.pSignalSemaphoreInfos    = signalSemInfos;
    VK_CHECK(vkQueueSubmit2(m_device->getGraphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE),
             "Queue submit failed");

    VkSemaphore renderFinished = m_sync->getRenderFinishedSemaphore(imageIndex);
    VkSwapchainKHR swapchains[] = { m_swapchain->getSwapchain() };
    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &renderFinished;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = swapchains;
    pi.pImageIndices      = &imageIndex;
    result = vkQueuePresentKHR(m_device->getPresentQueue(), &pi);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_window.wasResized()) {
        m_window.resetResizedFlag();
        recreateSwapchain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to present");
    }

    m_currentFrame = (m_currentFrame + 1) % Sync::MAX_FRAMES_IN_FLIGHT;
}
// ── buildFrameContext ────────────────────────────────────────────────────────
FrameContext Renderer::buildFrameContext(VkCommandBuffer cmd, uint32_t imageIndex) {
    auto ext = m_swapchain->getExtent();
    float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);

    FrameContext ctx{};
    ctx.frameIndex  = m_currentFrame;
    ctx.imageIndex  = imageIndex;
    ctx.cmd         = cmd;
    ctx.extent      = ext;
    ctx.aspect      = aspect;
    ctx.dt          = m_currentDt;
    ctx.gameTime    = m_gameTime;

    ctx.view      = m_isoCam.getViewMatrix();
    ctx.proj      = m_isoCam.getProjectionMatrix(aspect);
    ctx.viewProj  = ctx.proj * ctx.view;
    ctx.cameraPos = m_isoCam.getPosition();
    ctx.nearPlane = m_isoCam.getNear();
    ctx.farPlane  = m_isoCam.getFar();
    ctx.frustum.update(ctx.viewProj);

    ctx.device             = m_device.get();
    ctx.descriptors        = m_descriptors.get();
    ctx.bindless           = m_bindless.get();
    ctx.pipeline           = m_pipeline.get();
    ctx.hdrFB              = m_hdrFB.get();
    ctx.swapchain          = m_swapchain.get();
    ctx.shadowPass         = &m_shadowPass;
    ctx.bloom              = m_bloom.get();
    ctx.toneMap             = m_toneMap.get();
    ctx.inkingPass         = m_inkingPass.get();
    ctx.fogOfWar           = m_fogOfWar.get();
    ctx.outlineRenderer    = m_outlineRenderer.get();
    ctx.waterRenderer      = m_waterRenderer.get();
    ctx.distortionRenderer = m_distortionRenderer.get();
    ctx.shieldBubble       = m_shieldBubble.get();
    ctx.coneEffect         = m_coneEffect.get();
    ctx.explosionRenderer  = m_explosionRenderer.get();
    ctx.spriteEffects      = m_spriteEffectRenderer.get();
    ctx.clickIndicator     = m_clickIndicatorRenderer.get();
    ctx.groundDecals       = m_groundDecalRenderer.get();
    ctx.megaBuffer         = m_megaBuffer.get();
    ctx.hizPass            = &m_hizPass;
    ctx.vfxRenderer        = m_vfxRenderer.get();
    ctx.trailRenderer      = m_trailRenderer.get();
    ctx.meshEffects        = m_meshEffectRenderer.get();
    ctx.debugRenderer      = &m_debugRenderer;
    ctx.gpuTimer           = m_gpuTimer.get();
    ctx.scene              = &m_scene;
    ctx.threadPool         = &m_threadPool;
    ctx.cmdPools           = &m_cmdPools;
    ctx.asyncCompute       = &m_asyncCompute;
    ctx.ssaoPass           = &m_ssaoPass;
    ctx.ssrPass            = &m_ssrPass;

    ctx.skinnedPipeline       = m_skinnedPipeline;
    ctx.skinnedPipelineLayout = m_skinnedPipelineLayout;
    ctx.gridPipeline          = m_gridPipeline;
    ctx.gridPipelineLayout    = m_gridPipelineLayout;
    ctx.instanceBuffer        = m_instanceBuffers[m_currentFrame].getBuffer();
    ctx.instanceMapped        = static_cast<InstanceData*>(m_instanceMapped[m_currentFrame]);
    ctx.sceneMapped           = m_sceneMapped[m_currentFrame];
    ctx.renderAlpha           = m_renderAlpha;
    ctx.wireframe             = m_wireframe;
    ctx.showGrid              = m_showGrid;
    ctx.fogEnabled            = m_fogEnabled;
    ctx.isLauncher            = m_menuMode;

    return ctx;
}

// ── recordCommandBuffer ──────────────────────────────────────────────────────
void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, float /*dt*/) {
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi), "Begin command buffer");

    m_cmdPools.resetFrame(m_currentFrame);

    // ── Reset GPU timer queries for this frame ──────────────────────────────
    if (m_gpuTimer) m_gpuTimer->resetFrame(cmd, m_currentFrame);

    // ── Update per-frame UBOs ───────────────────────────────────────────────
    auto ext = m_swapchain->getExtent();
    float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);

    {
        glm::mat4 viewMat = m_isoCam.getViewMatrix();
        glm::mat4 projMat = m_isoCam.getProjectionMatrix(aspect);

        glm::vec3 lightDir = glm::normalize(glm::vec3(100.0f, 60.0f, 100.0f));
        m_shadowPass.updateCascades(viewMat, projMat, lightDir, 0.1f, 150.0f);
        const auto& cascades = m_shadowPass.getCascades();

        UniformBufferObject ubo{};
        ubo.view              = viewMat;
        ubo.proj              = projMat;
        ubo.lightSpaceMatrix  = cascades[0].lightViewProj;
        ubo.lightSpaceMatrix1 = cascades[1].lightViewProj;
        ubo.lightSpaceMatrix2 = cascades[2].lightViewProj;
        ubo.cascadeSplits     = glm::vec4(cascades[0].splitDepth,
                                          cascades[1].splitDepth,
                                          cascades[2].splitDepth, 0.0f);
        m_descriptors->updateUniformBuffer(m_currentFrame, ubo);
    }
    {
        LightUBO light{};
        light.viewPos = m_isoCam.getPosition();
        light.lightCount = 1;
        light.ambientStrength = 0.5f;
        light.lights[0].position = glm::vec3(100.0f, 60.0f, 100.0f);
        light.lights[0].color    = glm::vec3(1.0f, 0.95f, 0.85f);
        light.fogDensity = m_fogEnabled ? 0.015f : 0.0f;  // moderate — visible but not overpowering
        light.fogColor   = glm::vec3(0.15f, 0.18f, 0.25f); // dark blue-grey, LoL-style
        light.fogStart   = 30.0f;
        light.fogEnd     = 80.0f;
        light.appTime    = m_gameTime;
        m_descriptors->updateLightBuffer(m_currentFrame, light);
    }

    // ── Conditional pass enable/disable ─────────────────────────────────────
    bool isLauncher = m_menuMode;
    m_renderGraph.setPassEnabled("FogOfWar",      m_fogEnabled && !isLauncher);
    m_renderGraph.setPassEnabled("VFXAcquire",    !isLauncher);
    m_renderGraph.setPassEnabled("Shadow",        !isLauncher);
    m_renderGraph.setPassEnabled("TransparentVFX",!isLauncher);
    m_renderGraph.setPassEnabled("SSAO",          m_ssaoPass.isEnabled() && !isLauncher);
    m_renderGraph.setPassEnabled("SSR",           m_ssrPass.isEnabled() && !isLauncher);

    // ── Build per-frame context and execute the render graph ────────────────
    FrameContext ctx = buildFrameContext(cmd, imageIndex);
    m_renderGraph.execute(cmd, ctx);

    VK_CHECK(vkEndCommandBuffer(cmd), "End command buffer");
}

// ── recordShadowPass ─────────────────────────────────────────────────────────
void Renderer::recordShadowPass(VkCommandBuffer cmd, const FrameContext& ctx) {
    VkDescriptorSet ds = m_descriptors->getSet(m_currentFrame);

    // Pre-collect static entities for random-access indexing
    struct ShadowEntity { uint32_t meshIndex; glm::mat4 model; };
    std::vector<ShadowEntity> shadowEntities;
    {
        auto staticView = m_scene.getRegistry()
            .view<TransformComponent, MeshComponent, MaterialComponent>(
                entt::exclude<GPUSkinnedMeshComponent>);
        for (auto&& [e, t, mc, mat] : staticView.each()) {
            if (shadowEntities.size() >= MAX_INSTANCES) break;
            shadowEntities.push_back({mc.meshIndex, t.getInterpolatedModelMatrix(m_renderAlpha)});
        }
    }

    // Pre-fill instance buffer model matrices for shadow
    auto* instances = static_cast<InstanceData*>(m_instanceMapped[m_currentFrame]);
    for (uint32_t i = 0; i < shadowEntities.size(); ++i) {
        instances[i].model = shadowEntities[i].model;
    }

    if (m_gpuTimer) m_gpuTimer->beginZone(cmd, m_currentFrame, "Shadow");

    if (shadowEntities.size() >= PARALLEL_THRESHOLD) {
        auto shadowFormats = RenderFormats::depthOnly(ShadowPass::DEPTH_FORMAT);
        VkBuffer instBuf = m_instanceBuffers[m_currentFrame].getBuffer();

        auto parallelStaticFn = [&](uint32_t /*cascade*/, const glm::mat4& lvp,
                                    VkViewport vp, VkRect2D sc) -> std::vector<VkCommandBuffer> {
            auto result = ParallelRecorder::record(
                m_threadPool, m_cmdPools, m_currentFrame, shadowFormats,
                static_cast<uint32_t>(shadowEntities.size()),
                [&](VkCommandBuffer scmd, uint32_t start, uint32_t end) {
                    vkCmdSetViewport(scmd, 0, 1, &vp);
                    vkCmdSetScissor(scmd, 0, 1, &sc);
                    vkCmdBindPipeline(scmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_shadowPass.getStaticPipeline());
                    vkCmdPushConstants(scmd, m_shadowPass.getPipelineLayout(),
                                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &lvp);
                    for (uint32_t i = start; i < end; ++i) {
                        VkDeviceSize instOffset = i * sizeof(InstanceData);
                        vkCmdBindVertexBuffers(scmd, 1, 1, &instBuf, &instOffset);
                        m_scene.getMesh(shadowEntities[i].meshIndex).draw(scmd);
                    }
                });
            return result.secondaryBuffers;
        };

        m_shadowPass.recordCommandsParallel(cmd, parallelStaticFn, nullptr);
    } else {
        auto drawStaticShadows = [&](VkCommandBuffer scmd, uint32_t /*cascade*/) {
            VkBuffer instBuf = m_instanceBuffers[m_currentFrame].getBuffer();
            for (uint32_t i = 0; i < static_cast<uint32_t>(shadowEntities.size()); ++i) {
                VkDeviceSize instOffset = i * sizeof(InstanceData);
                vkCmdBindVertexBuffers(scmd, 1, 1, &instBuf, &instOffset);
                m_scene.getMesh(shadowEntities[i].meshIndex).draw(scmd);
            }
        };
        m_shadowPass.recordCommands(cmd, drawStaticShadows, nullptr);
    }

    if (m_gpuTimer) m_gpuTimer->endZone(cmd, m_currentFrame, "Shadow");
}

// ── recordGBufferPass ────────────────────────────────────────────────────────
void Renderer::recordGBufferPass(VkCommandBuffer cmd, const FrameContext& ctx) {
    auto ext = ctx.extent;
    float aspect = ctx.aspect;

    // ── Transition HDR attachments to attachment-optimal layouts ─────────────
    {
        VkImageMemoryBarrier2 barriers[3]{};
        barriers[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barriers[0].srcAccessMask = VK_ACCESS_2_NONE;
        barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[0].image = m_hdrFB->colorImage();
        barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        barriers[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barriers[1].srcAccessMask = VK_ACCESS_2_NONE;
        barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barriers[1].image = m_hdrFB->depthImage();
        barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};

        barriers[2] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[2].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barriers[2].srcAccessMask = VK_ACCESS_2_NONE;
        barriers[2].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[2].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[2].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[2].image = m_hdrFB->charDepthImage();
        barriers[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 3;
        depInfo.pImageMemoryBarriers    = barriers;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }

    VkRenderingAttachmentInfo hdrColorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    hdrColorAttach.imageView   = m_hdrFB->colorView();
    hdrColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hdrColorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    hdrColorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    hdrColorAttach.clearValue.color = {{ 0.08f, 0.10f, 0.14f, 1.0f }};

    VkRenderingAttachmentInfo hdrCharDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    hdrCharDepthAttach.imageView   = m_hdrFB->characterDepthView();
    hdrCharDepthAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hdrCharDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    hdrCharDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    hdrCharDepthAttach.clearValue.color = {{ 0.0f, 0.0f, 0.0f, 0.0f }};

    VkRenderingAttachmentInfo hdrColorAttachments[2] = {hdrColorAttach, hdrCharDepthAttach};

    VkRenderingAttachmentInfo hdrDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    hdrDepthAttach.imageView   = m_hdrFB->depthAttachmentView();
    hdrDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    hdrDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    hdrDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    hdrDepthAttach.clearValue.depthStencil = { 0.0f, 0 }; // reversed-Z: far = 0.0

    VkRenderingInfo hdrRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    hdrRenderInfo.renderArea          = { {0, 0}, ext };
    hdrRenderInfo.layerCount          = 1;
    hdrRenderInfo.colorAttachmentCount = 2;
    hdrRenderInfo.pColorAttachments   = hdrColorAttachments;
    hdrRenderInfo.pDepthAttachment    = &hdrDepthAttach;
    hdrRenderInfo.pStencilAttachment  = &hdrDepthAttach;

    VkViewport vp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
    VkRect2D   sc{ {0, 0}, ext };

    // ── Phase 1: CPU data fill (sequential, before rendering) ───────────────
    struct SkinnedDrawInfo {
        entt::entity entity;
        VkBuffer     instBuf;
        VkDeviceSize instOffset;
        uint32_t     boneBase;
        uint32_t     meshIdx;
    };
    // Use frame allocator instead of std::vector for transient draw list
    constexpr uint32_t MAX_SKINNED_DRAWS = 512;
    auto* skinnedDrawData = m_frameAllocator.alloc<SkinnedDrawInfo>(MAX_SKINNED_DRAWS);
    uint32_t skinnedDrawCount = 0;
    uint32_t objectCount   = 0;
    uint32_t instanceIndex = 0;

    if (!m_menuMode) {
        auto* instances = static_cast<InstanceData*>(m_instanceMapped[m_currentFrame]);
        auto* sceneData = static_cast<GpuObjectData*>(m_sceneMapped[m_currentFrame]);
        VkDescriptorSet ds = m_descriptors->getSet(m_currentFrame);

        // ── Static entities ─────────────────────────────────────────────────
        m_impostorSystem.beginFrame();
        glm::vec3 camPos = m_isoCam.getPosition();

        auto staticView = m_scene.getRegistry()
            .view<TransformComponent, MeshComponent, MaterialComponent>(
                entt::exclude<GPUSkinnedMeshComponent>);
        for (auto&& [e, t, mc, mat] : staticView.each()) {
            if (objectCount >= MAX_INSTANCES) break;

            // FoW: skip enemies not in vision
            if (!FogOfWarGameplay::shouldRender(m_scene.getRegistry(), e, Team::PLAYER))
                continue;

            glm::mat4 model = t.getInterpolatedModelMatrix(m_renderAlpha);
            glm::vec3 worldPos = glm::vec3(model[3]);
            float dist = glm::length(worldPos - camPos);

            glm::vec4 tint(1.0f);
            if (e == m_hoveredEntity) tint = glm::vec4(1.0f, 0.4f, 0.4f, 1.0f);
            else if (auto* tc = m_scene.getRegistry().try_get<TintComponent>(e)) tint = tc->color;

            // FoW fade: modulate alpha for enemies fading in/out
            float fowAlpha = FogOfWarGameplay::getRenderAlpha(m_scene.getRegistry(), e);
            tint.a *= fowAlpha;

            // Beyond impostor distance → render as billboard, skip mesh
            if (m_lodSystem.shouldBeImpostor(dist)) {
                auto inst = m_impostorSystem.buildInstance(
                    "default", worldPos, camPos, tint);
                m_impostorSystem.addInstance(inst);
                continue;
            }

            uint32_t mi = mc.meshIndex;
            if (mi >= m_meshHandles.size()) continue;

            // Determine sub-mesh range: -1 means draw all sub-meshes
            uint32_t subCount = static_cast<uint32_t>(m_meshHandles[mi].size());
            uint32_t siStart  = (mc.subMeshIndex < 0) ? 0 : static_cast<uint32_t>(mc.subMeshIndex);
            uint32_t siEnd    = (mc.subMeshIndex < 0) ? subCount : siStart + 1;

            glm::mat4 normalMat = glm::transpose(glm::inverse(model));
            AABB worldAABB = mc.localAABB.transformed(model);
            glm::vec4 params(mat.shininess, mat.metallic, mat.roughness, mat.emissive);

            for (uint32_t si = siStart; si < siEnd && objectCount < MAX_INSTANCES; ++si) {
                // Per-sub-mesh texture: prefer subMeshAlbedo[si] when available
                uint32_t diffuseTex = (si < mat.subMeshAlbedo.size())
                    ? mat.subMeshAlbedo[si]
                    : mat.materialIndex;

                instances[objectCount].model        = model;
                instances[objectCount].normalMatrix = normalMat;
                instances[objectCount].tint         = tint;
                instances[objectCount].params       = params;
                instances[objectCount].texIndices   = glm::vec4(
                    static_cast<float>(diffuseTex),
                    static_cast<float>(mat.normalMapIndex), 0.0f, 0.0f);

                const auto& mh = m_meshHandles[mi][si];
                LODLevel lod = m_lodSystem.getLODLevel(mi, si, dist);
                uint32_t vOff = (lod.indexCount > 0) ? lod.vertexOffset : mh.vertexOffset;
                uint32_t iOff = (lod.indexCount > 0) ? lod.indexOffset  : mh.indexOffset;
                uint32_t iCnt = (lod.indexCount > 0) ? lod.indexCount   : mh.indexCount;

                auto& obj = sceneData[objectCount];
                obj.model            = model;
                obj.normalMatrix     = normalMat;
                obj.aabbMin          = glm::vec4(worldAABB.min, 0.0f);
                obj.aabbMax          = glm::vec4(worldAABB.max, 0.0f);
                obj.tint             = tint;
                obj.params           = params;
                obj.texIndices       = glm::vec4(
                    static_cast<float>(diffuseTex),
                    static_cast<float>(mat.normalMapIndex), 0.0f, 0.0f);
                obj.meshVertexOffset = vOff;
                obj.meshIndexOffset  = iOff;
                obj.meshIndexCount   = iCnt;
                obj._pad             = 0;

                ++objectCount;
            }
        }
        instanceIndex = objectCount;

        // Flush scene buffer descriptor
        if (objectCount > 0) {
            VkDeviceSize usedSize = objectCount * sizeof(GpuObjectData);
            m_sceneBuffers[m_currentFrame].flush();
            m_descriptors->writeSceneBuffer(m_currentFrame,
                m_sceneBuffers[m_currentFrame].getBuffer(), usedSize);
        }

        // ── Skinned entities ────────────────────────────────────────────────
        auto skinnedView = m_scene.getRegistry()
            .view<TransformComponent, MaterialComponent, GPUSkinnedMeshComponent>();
        for (auto&& [e, t, mat, ssm] : skinnedView.each()) {
            if (instanceIndex >= MAX_INSTANCES) break;

            // FoW: skip enemies not in vision
            if (!FogOfWarGameplay::shouldRender(m_scene.getRegistry(), e, Team::PLAYER))
                continue;

            glm::mat4 model = t.getInterpolatedModelMatrix(m_renderAlpha);
            instances[instanceIndex].model        = model;
            instances[instanceIndex].normalMatrix = glm::transpose(glm::inverse(model));

            glm::vec4 tint(1.0f);
            if (e == m_hoveredEntity) {
                tint = glm::vec4(1.0f, 0.6f, 0.6f, 1.0f);
            } else if (auto* tc = m_scene.getRegistry().try_get<TintComponent>(e)) {
                tint = tc->color;
            }

            glm::vec3 worldPos = t.position;
            float dist = glm::distance(worldPos, camPos);

            // FoW fade: modulate alpha for enemies fading in/out
            float fowAlpha = FogOfWarGameplay::getRenderAlpha(m_scene.getRegistry(), e);
            tint.a *= fowAlpha;

            // Beyond impostor distance → render as billboard, skip mesh
            if (m_lodSystem.shouldBeImpostor(dist)) {
                std::string type = "minion";
                if (auto* rc = m_scene.getRegistry().try_get<RespawnComponent>(e)) {
                    if (rc->isHero) type = "hero";
                }
                auto inst = m_impostorSystem.buildInstance(type, worldPos, camPos, tint);
                m_impostorSystem.addInstance(inst);
                continue;
            }

            instances[instanceIndex].tint = tint;
            instances[instanceIndex].params = glm::vec4(mat.shininess, mat.metallic, mat.roughness, mat.emissive);
            instances[instanceIndex].texIndices = glm::vec4(
                static_cast<float>(mat.materialIndex), static_cast<float>(mat.normalMapIndex), 0.0f, 0.0f);

            VkBuffer     instBuf    = m_instanceBuffers[m_currentFrame].getBuffer();
            VkDeviceSize instOffset = instanceIndex * sizeof(InstanceData);
            uint32_t boneBase = ssm.boneSlot * Descriptors::MAX_BONES;

            if (skinnedDrawCount < MAX_SKINNED_DRAWS)
                skinnedDrawData[skinnedDrawCount++] = {e, instBuf, instOffset, boneBase, ssm.staticSkinnedMeshIndex};
            ++instanceIndex;
        }
    }

    // ── Phase 2: HDR Scope 1 — parallel geometry draws ──────────────────────
    if (m_gpuTimer) m_gpuTimer->beginZone(cmd, m_currentFrame, "Geometry");

    if (!m_menuMode) {
        VkDescriptorSet ds = m_descriptors->getSet(m_currentFrame);
        auto* sceneData = static_cast<GpuObjectData*>(m_sceneMapped[m_currentFrame]);
        RenderFormats hdrFormats = m_hdrFB->mainFormats();

        // Record static mesh draws in parallel
        ParallelRecordResult staticResult;
        if (objectCount > 0) {
            staticResult = ParallelRecorder::record(
                m_threadPool, m_cmdPools, m_currentFrame, hdrFormats, objectCount,
                [&](VkCommandBuffer scmd, uint32_t start, uint32_t end) {
                    VkViewport lvp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
                    VkRect2D lsc{ {0,0}, ext };
                    vkCmdSetViewport(scmd, 0, 1, &lvp);
                    vkCmdSetScissor(scmd, 0, 1, &lsc);
                    VkPipeline mainPipeline = m_wireframe ? m_pipeline->getWireframePipeline()
                                                          : m_pipeline->getGraphicsPipeline();
                    vkCmdBindPipeline(scmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mainPipeline);
                    VkDescriptorSet sets[2] = { ds, m_bindless->getSet() };
                    vkCmdBindDescriptorSets(scmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_pipeline->getPipelineLayout(), 0, 2, sets, 0, nullptr);
                    m_megaBuffer->bind(scmd);
                    for (uint32_t i = start; i < end; ++i) {
                        vkCmdDrawIndexed(scmd, sceneData[i].meshIndexCount, 1,
                            sceneData[i].meshIndexOffset,
                            static_cast<int32_t>(sceneData[i].meshVertexOffset), i);
                    }
                });
        }

        // Record skinned mesh draws in parallel
        ParallelRecordResult skinnedResult;
        if (skinnedDrawCount > 0) {
            skinnedResult = ParallelRecorder::record(
                m_threadPool, m_cmdPools, m_currentFrame, hdrFormats,
                skinnedDrawCount,
                [&](VkCommandBuffer scmd, uint32_t start, uint32_t end) {
                    VkViewport lvp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
                    VkRect2D lsc{ {0,0}, ext };
                    vkCmdSetViewport(scmd, 0, 1, &lvp);
                    vkCmdSetScissor(scmd, 0, 1, &lsc);
                    vkCmdBindPipeline(scmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
                    VkDescriptorSet sets[2] = { ds, m_bindless->getSet() };
                    vkCmdBindDescriptorSets(scmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_skinnedPipelineLayout, 0, 2, sets, 0, nullptr);
                    for (uint32_t i = start; i < end; ++i) {
                        auto& info = skinnedDrawData[i];
                        vkCmdBindVertexBuffers(scmd, 1, 1, &info.instBuf, &info.instOffset);
                        vkCmdPushConstants(scmd, m_skinnedPipelineLayout,
                                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                           0, sizeof(uint32_t), &info.boneBase);
                        const auto& mesh = m_scene.getStaticSkinnedMesh(info.meshIdx);
                        mesh.bind(scmd);
                        mesh.draw(scmd);
                    }
                });
        }

        bool hasSecondaries = !staticResult.secondaryBuffers.empty() ||
                              !skinnedResult.secondaryBuffers.empty();

        if (hasSecondaries)
            hdrRenderInfo.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;

        vkCmdBeginRendering(cmd, &hdrRenderInfo);
        if (!staticResult.secondaryBuffers.empty())
            vkCmdExecuteCommands(cmd, static_cast<uint32_t>(staticResult.secondaryBuffers.size()),
                                 staticResult.secondaryBuffers.data());
        if (!skinnedResult.secondaryBuffers.empty())
            vkCmdExecuteCommands(cmd, static_cast<uint32_t>(skinnedResult.secondaryBuffers.size()),
                                 skinnedResult.secondaryBuffers.data());
        vkCmdEndRendering(cmd);

        // ── Self-synchronizing barrier between scope 1 and scope 2 ──────────
        {
            VkMemoryBarrier2 memBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            memBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                     | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            memBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                                     | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            memBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                     | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
            memBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                                     | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT
                                     | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                     | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &memBarrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        // ── Phase 3: HDR Scope 2 (inline, LOAD_OP_LOAD) — outlines + extras ─
        {
            VkRenderingAttachmentInfo loadColorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            loadColorAttach.imageView   = m_hdrFB->colorView();
            loadColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            loadColorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
            loadColorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingAttachmentInfo loadCharDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            loadCharDepthAttach.imageView   = m_hdrFB->characterDepthView();
            loadCharDepthAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            loadCharDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
            loadCharDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingAttachmentInfo loadColorAttachments[2] = {loadColorAttach, loadCharDepthAttach};

            VkRenderingAttachmentInfo loadDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            loadDepthAttach.imageView   = m_hdrFB->depthAttachmentView();
            loadDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            loadDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
            loadDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo loadRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
            loadRenderInfo.renderArea          = { {0, 0}, ext };
            loadRenderInfo.layerCount          = 1;
            loadRenderInfo.colorAttachmentCount = 2;
            loadRenderInfo.pColorAttachments   = loadColorAttachments;
            loadRenderInfo.pDepthAttachment    = &loadDepthAttach;
            loadRenderInfo.pStencilAttachment  = &loadDepthAttach;

            vkCmdBeginRendering(cmd, &loadRenderInfo);

            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);

            VkDescriptorSet sets[2] = { ds, m_bindless->getSet() };

            // ── Team-colored outlines (LoL Stage 8) ─────────────────────────
            if (m_outlineRenderer && skinnedDrawCount > 0) {
                auto& reg = m_scene.getRegistry();

                entt::entity attackTarget = entt::null;
                if (reg.valid(m_playerEntity) &&
                    reg.all_of<CombatComponent>(m_playerEntity)) {
                    attackTarget = reg.get<CombatComponent>(m_playerEntity).targetEntity;
                    if (!reg.valid(attackTarget)) attackTarget = entt::null;
                }

                for (uint32_t si = 0; si < skinnedDrawCount; ++si) {
                    const auto& info = skinnedDrawData[si];
                    entt::entity e      = info.entity;
                    bool isHovered      = (e == m_hoveredEntity);
                    bool isSelected     = reg.all_of<SelectableComponent>(e) &&
                                          reg.get<SelectableComponent>(e).isSelected;
                    bool isAttackTarget = (e == attackTarget);

                    const auto& mesh = m_scene.getStaticSkinnedMesh(info.meshIdx);

                    if (isHovered) {
                        m_outlineRenderer->renderOutline(
                            cmd, ds, info.instBuf, info.instOffset,
                            info.boneBase, mesh,
                            0.035f, glm::vec4(1.0f, 0.85f, 0.0f, 1.0f)); // gold
                    } else if (isAttackTarget) {
                        m_outlineRenderer->renderOutline(
                            cmd, ds, info.instBuf, info.instOffset,
                            info.boneBase, mesh,
                            0.030f, glm::vec4(1.0f, 0.25f, 0.1f, 1.0f)); // enemy red
                    } else if (isSelected) {
                        m_outlineRenderer->renderOutline(
                            cmd, ds, info.instBuf, info.instOffset,
                            info.boneBase, mesh,
                            0.030f, glm::vec4(0.2f, 0.5f, 1.0f, 1.0f)); // blue team
                    }
                }

                // Restore the skinned pipeline for anything that follows
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_skinnedPipelineLayout, 0, 2, sets, 0, nullptr);
            }

            // ── Debug grid ──────────────────────────────────────────────────
            if (m_showGrid && m_gridPipeline != VK_NULL_HANDLE) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
                struct GridPC { glm::mat4 viewProj; float gridY; } gridPC;
                gridPC.viewProj = ctx.viewProj;
                gridPC.gridY    = 0.0f;
                vkCmdPushConstants(cmd, m_gridPipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(GridPC), &gridPC);
                vkCmdDraw(cmd, 6, 1, 0, 0);
            }

            // ── Ground decals (depth test ON, depth write OFF, alpha blend) ──
            if (m_groundDecalRenderer) {
                float appTime = static_cast<float>(glfwGetTime());
                m_groundDecalRenderer->render(cmd, ctx.viewProj, appTime);
            }

            // ── Click indicator ─────────────────────────────────────────────
            if (m_clickAnim && m_clickIndicatorRenderer) {
                float t_norm = m_clickAnim->lifetime / m_clickAnim->maxLife;
                m_clickIndicatorRenderer->render(cmd, ctx.viewProj, m_clickAnim->position, t_norm, 1.5f);
            }

            // ── Debug Renderer (Marquee Box) ────────────────────────────────
            m_debugRenderer.render(cmd, ctx.viewProj);

            // ── Water surface (alpha-blend, depth-test ON, depth-write OFF) ──
            if (m_waterRenderer) {
                float appTime = static_cast<float>(glfwGetTime());
                glm::mat4 waterModel = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.25f, 0.0f))
                                     * glm::scale(glm::mat4(1.0f), glm::vec3(200.0f, 1.0f, 200.0f));
                m_waterRenderer->render(cmd, m_descriptors->getSet(m_currentFrame),
                                        m_bindless->getSet(), appTime, waterModel);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_skinnedPipelineLayout, 0, 2, sets, 0, nullptr);
            }

            vkCmdEndRendering(cmd);
        }
    } else {
        // Launcher mode: just clear the HDR framebuffer
        vkCmdBeginRendering(cmd, &hdrRenderInfo);
        vkCmdEndRendering(cmd);
    }

    if (m_gpuTimer) m_gpuTimer->endZone(cmd, m_currentFrame, "Geometry");

    // ── Transition HDR attachments to read-only layouts ─────────────────────
    {
        VkImageMemoryBarrier2 barriers[3]{};
        barriers[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].image = m_hdrFB->colorImage();
        barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        barriers[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        barriers[1].image = m_hdrFB->depthImage();
        barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};

        barriers[2] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barriers[2].srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[2].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[2].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[2].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barriers[2].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[2].image = m_hdrFB->charDepthImage();
        barriers[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 3;
        depInfo.pImageMemoryBarriers    = barriers;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }
}

// ── recordTransparentVFXPass ─────────────────────────────────────────────────
void Renderer::recordTransparentVFXPass(VkCommandBuffer cmd, const FrameContext& ctx) {
    auto ext = ctx.extent;
    float aspect = ctx.aspect;

    // Copy scene color for distortion effects before rendering transparents over it
    bool copiedColor = false;
    if (m_distortionRenderer) {
        m_hdrFB->copyColor(cmd);
        copiedColor = true;
    }

    // Transition color back to attachment for load pass writing
    // (copyColor already leaves color in COLOR_ATTACHMENT_OPTIMAL — skip if done)
    if (!copiedColor) {
        VkImageMemoryBarrier2 colorToAttach{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        colorToAttach.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        colorToAttach.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        colorToAttach.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        colorToAttach.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        colorToAttach.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        colorToAttach.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorToAttach.image = m_hdrFB->colorImage();
        colorToAttach.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers    = &colorToAttach;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }

    // Load pass: color LOAD (preserve geometry), charDepth read-only (inking reads it),
    // depth read-only (depth test, no write)
    VkRenderingAttachmentInfo loadColorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    loadColorAttach.imageView   = m_hdrFB->colorView();
    loadColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    loadColorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    loadColorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingAttachmentInfo loadCharDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    loadCharDepthAttach.imageView   = m_hdrFB->characterDepthView();
    loadCharDepthAttach.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    loadCharDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    loadCharDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_NONE;

    VkRenderingAttachmentInfo loadColorAttachments[2] = {loadColorAttach, loadCharDepthAttach};

    VkRenderingAttachmentInfo loadDepthAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    loadDepthAttach.imageView   = m_hdrFB->depthAttachmentView();
    loadDepthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    loadDepthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
    loadDepthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_NONE;

    VkRenderingInfo loadRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    loadRenderInfo.renderArea           = { {0, 0}, ext };
    loadRenderInfo.layerCount           = 1;
    loadRenderInfo.colorAttachmentCount = 2;
    loadRenderInfo.pColorAttachments    = loadColorAttachments;
    loadRenderInfo.pDepthAttachment     = &loadDepthAttach;
    loadRenderInfo.pStencilAttachment   = &loadDepthAttach;

    vkCmdBeginRendering(cmd, &loadRenderInfo);
    if (m_gpuTimer) m_gpuTimer->beginZone(cmd, m_currentFrame, "VFX/Trans");

    VkViewport vp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
    VkRect2D   sc{ {0, 0}, ext };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // ── Character outline (Sobel inking pass) ───────────────────────────────
    if (m_inkingPass) {
        m_inkingPass->render(cmd, 0.02f, 1.5f, glm::vec4(0.05f, 0.03f, 0.02f, 1.0f));
    }

    glm::mat4 viewProj = ctx.viewProj;

    // ── VFX particle render pass ────────────────────────────────────────────
    if (m_vfxRenderer) {
        const glm::mat4& view = m_isoCam.getViewMatrix();
        glm::vec3 camRight{view[0][0], view[1][0], view[2][0]};
        glm::vec3 camUp   {view[0][1], view[1][1], view[2][1]};
        glm::vec2 screenSize(static_cast<float>(ext.width), static_cast<float>(ext.height));
        m_vfxRenderer->render(cmd, viewProj, camRight, camUp,
                              screenSize, m_isoCam.getNear(), m_isoCam.getFar());
    }

    // ── Trail ribbon render pass ────────────────────────────────────────────
    if (m_trailRenderer) {
        const glm::mat4& view = m_isoCam.getViewMatrix();
        glm::vec3 camRight{view[0][0], view[1][0], view[2][0]};
        glm::vec3 camUp   {view[0][1], view[1][1], view[2][1]};
        m_trailRenderer->render(cmd, viewProj, camRight, camUp);
    }

    // ── Glass shield bubble ─────────────────────────────────────────────────
    if (m_shieldBubble) {
        auto& reg = m_scene.getRegistry();
        if (reg.all_of<CombatComponent, TransformComponent>(m_playerEntity)) {
            const auto& combat = reg.get<CombatComponent>(m_playerEntity);
            if (combat.state == CombatState::SHIELDING) {
                const auto& t = reg.get<TransformComponent>(m_playerEntity);
                float elapsed  = combat.shieldDuration - combat.stateTimer;
                float fadeIn   = std::min(elapsed / 0.25f, 1.0f);
                float fadeOut  = std::min(combat.stateTimer / 0.25f, 1.0f);
                float alpha    = fadeIn * fadeOut;
                glm::vec3 center    = t.position + glm::vec3(0.0f, 2.5f, 0.0f);
                glm::vec3 cameraPos = m_isoCam.getPosition();
                float appTime = static_cast<float>(glfwGetTime());
                m_shieldBubble->render(cmd, viewProj, center, cameraPos,
                                       3.2f, appTime, alpha);
            }
        }
    }

    // ── E-ability cone effect (Molten Shield) ───────────────────────────────
    if (m_coneEffectTimer > 0.0f && m_coneEffect) {
        float elapsed    = CONE_DURATION - m_coneEffectTimer;
        glm::vec3 camPos = m_isoCam.getPosition();
        float     t      = static_cast<float>(glfwGetTime());
        m_coneEffect->render(cmd, viewProj, m_coneApex, m_coneDirection,
                             CONE_HALF_ANGLE, CONE_RANGE,
                             camPos, t, elapsed, 1.0f);
    }

    // ── R-ability explosion effects (Incendiary Bomb) ───────────────────────
    if (m_explosionRenderer) {
        glm::vec3 camPos = m_isoCam.getPosition();
        float appTime    = static_cast<float>(glfwGetTime());
        m_explosionRenderer->render(cmd, viewProj, camPos, appTime);
    }

    if (m_meshEffectRenderer) {
        float appTime = static_cast<float>(glfwGetTime());
        m_meshEffectRenderer->render(cmd, viewProj, appTime);
    }

    // ── Sprite-atlas VFX effects (W cone, E explosion) ──────────────────────
    if (m_spriteEffectRenderer) {
        glm::vec3 camPos = m_isoCam.getPosition();
        m_spriteEffectRenderer->render(cmd, viewProj, camPos);
    }

    // ── Distortion pass (rendered on top of transparents) ───────────────────
    if (m_distortionRenderer) {
        float appTime = static_cast<float>(glfwGetTime());
        m_distortionRenderer->render(cmd, viewProj, appTime, ext.width, ext.height);
    }

    if (m_gpuTimer) m_gpuTimer->endZone(cmd, m_currentFrame, "VFX/Trans");
    vkCmdEndRendering(cmd);

    // Transition HDR color to SHADER_READ_ONLY for bloom/tonemap sampling
    {
        VkImageMemoryBarrier2 colorBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        colorBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        colorBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        colorBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        colorBarrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        colorBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        colorBarrier.image = m_hdrFB->colorImage();
        colorBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers    = &colorBarrier;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }
}

// ── recordTonemapPass ────────────────────────────────────────────────────────
void Renderer::recordTonemapPass(VkCommandBuffer cmd, const FrameContext& ctx) {
    auto ext = ctx.extent;

    // ── Transition swapchain image to COLOR_ATTACHMENT_OPTIMAL ───────────────
    {
        VkImageMemoryBarrier2 swapBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        swapBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        swapBarrier.srcAccessMask = VK_ACCESS_2_NONE;
        swapBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        swapBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        swapBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        swapBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        swapBarrier.image = m_swapchain->getImages()[ctx.imageIndex];
        swapBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers    = &swapBarrier;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }

    VkRenderingAttachmentInfo swapColorAttach{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    swapColorAttach.imageView   = m_swapchain->getImageViews()[ctx.imageIndex];
    swapColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    swapColorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    swapColorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    swapColorAttach.clearValue.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};

    VkRenderingInfo swapRenderInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    swapRenderInfo.renderArea          = { {0, 0}, ext };
    swapRenderInfo.layerCount          = 1;
    swapRenderInfo.colorAttachmentCount = 1;
    swapRenderInfo.pColorAttachments   = &swapColorAttach;

    vkCmdBeginRendering(cmd, &swapRenderInfo);
    if (m_gpuTimer) m_gpuTimer->beginZone(cmd, m_currentFrame, "Tonemap");

    {
        VkViewport vp{0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1};
        VkRect2D   sc{{0, 0}, ext};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }
    float deathDesat = 0.0f;
    if (RespawnSystem::isDead(m_scene.getRegistry(), m_playerEntity))
        deathDesat = 0.7f; // heavy desaturation for death screen
    m_toneMap->render(cmd, /*exposure=*/1.0f, /*bloomStrength=*/0.3f,
                      /*vignette=*/1, /*colorGrade=*/1, /*chromatic=*/0.003f,
                      deathDesat);

    if (m_gpuTimer) m_gpuTimer->endZone(cmd, m_currentFrame, "Tonemap");

    // ── ImGui render draw data (last thing inside render pass) ──────────────
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    vkCmdEndRendering(cmd);

    // Transition swapchain image to PRESENT_SRC_KHR for presentation
    {
        VkImageMemoryBarrier2 presentBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        presentBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        presentBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        presentBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_NONE;
        presentBarrier.dstAccessMask = VK_ACCESS_2_NONE;
        presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        presentBarrier.image = m_swapchain->getImages()[ctx.imageIndex];
        presentBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo depInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        depInfo.imageMemoryBarrierCount = 1;
        depInfo.pImageMemoryBarriers    = &presentBarrier;
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }
}


// ── buildScene ───────────────────────────────────────────────────────────────
void Renderer::buildScene() {
    // ── Load map data for minimap & spawn system ─────────────────────────
    try {
        m_mapData = MapLoader::LoadFromFile(std::string(ASSET_DIR) + "maps/map_summonersrift.json");
        spdlog::info("Map '{}' loaded ({} towers per team)", m_mapData.mapName,
                     m_mapData.teams[0].towers.size());
    } catch (const std::exception& e) {
        spdlog::warn("Could not load map JSON: {} — using default bounds", e.what());
        // m_mapData keeps its default values (200×200, center 100,0,100)
    }

    // ── Navigation: init flow fields from map lane endpoints ─────────────
    {
        NavMeshData emptyNav;  // stub navmesh for now
        m_dynamicObstacles.init();
        m_pathfinding.init(emptyNav);
        m_pathfinding.initFlowFields(m_mapData);
        spdlog::info("[Renderer] Flow fields ready for {} lanes x 2 teams", 3);
    }

    // ── Spawn MOBA structures (towers, inhibitors, nexus) ────────────────
    {
        m_structureSystem->setVictoryCallback([this](uint8_t winningTeam) {
            spdlog::info("[Renderer] Victory callback: team {} wins!", winningTeam);
            if (m_onVictory) m_onVictory(winningTeam);
        });
        auto result = m_structureSystem->spawnStructures(m_scene.getRegistry(), m_mapData);
        int totalTowers = 0, totalInhibitors = 0;
        for (int t = 0; t < 2; ++t) {
            totalInhibitors += static_cast<int>(result.inhibitors[t].size());
            for (auto& lane : result.laneTowers[t])
                for (auto e : lane) if (e != entt::null) totalTowers++;
            totalTowers += static_cast<int>(result.nexusTowers[t].size());
        }
        spdlog::info("[Renderer] Structures spawned: {} towers, {} inhibitors, 2 nexus",
                     totalTowers, totalInhibitors);
    }

    // ── Init minion wave system with map data + pathfinding ─────────────
    m_minionWaveSystem->init(m_mapData);
    m_minionWaveSystem->setPathfinding(&m_pathfinding);
    m_minionWaveSystem->setScene(&m_scene);

    // ── Init respawn system ──────────────────────────────────────────────
    m_respawnSystem->init(m_mapData, m_economySystem.get(), m_audioEvents.get());
    m_respawnSystem->onRespawn = [this](entt::entity e, glm::vec3 fountain) {
        if (e == m_playerEntity) {
            m_isoCam.setFollowTarget(fountain);
            m_isoCam.setAttached(true);
        }
    };

    // Default textures
    uint32_t defaultTex  = m_scene.addTexture(Texture::createDefault(*m_device));
    uint32_t checkerTex  = m_scene.addTexture(Texture::createCheckerboard(*m_device));
    uint32_t flatNorm    = m_scene.addTexture(Texture::createFlatNormal(*m_device));
    m_flatNormIndex      = flatNorm;

    // Bind to bindless descriptor array
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_scene.getTextures().size()); ++i) {
        auto& tex = m_scene.getTexture(i);
        m_bindless->registerTexture(tex.getImageView(), tex.getSampler());
    }

    // ── Toon ramp texture (binding 5, 256×1 R8G8B8A8_UNORM) ─────────────
    // LoL-style gradient: dark cool shadow → midtone → bright warm highlight.
    // Zones: [0-39] shadow, [40-89] shadow→mid transition,
    //        [90-179] midtone, [180-219] mid→highlight transition, [220-255] highlight
    {
        constexpr uint32_t RAMP_W = 256;
        uint32_t pixels[RAMP_W];

        // Pack RGBA into a uint32_t for VK_FORMAT_R8G8B8A8_UNORM (little-endian)
        auto pack = [](float r, float g, float b) -> uint32_t {
            uint32_t R = static_cast<uint32_t>(r * 255.0f + 0.5f);
            uint32_t G = static_cast<uint32_t>(g * 255.0f + 0.5f);
            uint32_t B = static_cast<uint32_t>(b * 255.0f + 0.5f);
            return R | (G << 8) | (B << 16) | (0xFFu << 24);
        };

        // Anchor colours
        const float sR = 0.25f, sG = 0.22f, sB = 0.30f; // shadow
        const float mR = 0.75f, mG = 0.70f, mB = 0.68f; // midtone
        const float hR = 1.00f, hG = 0.98f, hB = 0.95f; // highlight

        for (uint32_t x = 0; x < RAMP_W; ++x) {
            if (x < 40) {
                pixels[x] = pack(sR, sG, sB);
            } else if (x < 90) {
                float t   = static_cast<float>(x - 40) / 50.0f;
                pixels[x] = pack(sR + t * (mR - sR), sG + t * (mG - sG), sB + t * (mB - sB));
            } else if (x < 180) {
                pixels[x] = pack(mR, mG, mB);
            } else if (x < 220) {
                float t   = static_cast<float>(x - 180) / 40.0f;
                pixels[x] = pack(mR + t * (hR - mR), mG + t * (hG - mG), mB + t * (hB - mB));
            } else {
                pixels[x] = pack(hR, hG, hB);
            }
        }

        m_toonRamp = Texture::createFromPixels(*m_device, pixels, RAMP_W, 1,
                                               VK_FORMAT_R8G8B8A8_UNORM);
        m_descriptors->writeToonRamp(m_toonRamp.getImageView(), m_toonRamp.getSampler());
        spdlog::info("Toon ramp texture created and bound to descriptor binding 5");
    }

    if (m_fogOfWar) {
        m_descriptors->writeFogOfWar(m_fogOfWar->getVisibilityView(), m_fogOfWar->getSampler());
        if (m_groundDecalRenderer) {
            // Decals sample FoW so ability indicators fade in unexplored areas
            m_groundDecalRenderer->setFogOfWar(
                m_fogOfWar->getVisibilityView(), m_fogOfWar->getSampler(),
                glm::vec2(0.0f, 0.0f), glm::vec2(200.0f, 200.0f));
        }
    }

    // ── Base ground plane (200×200, Y=0, below all lane tiles) ───────────
    {
        auto groundMesh = Model::createTerrain(*m_device, m_device->getAllocator(),
                                               200.0f, 1, 0.0f);
        uint32_t groundMeshIdx = m_scene.addMesh(std::move(groundMesh));

        auto groundEnt = m_scene.createEntity("ground_plane");
        auto& reg      = m_scene.getRegistry();
        reg.emplace<MeshComponent>(groundEnt, MeshComponent{ groundMeshIdx });
        // Matte green-brown terrain: no metallic, rough, slight warm tint
        reg.emplace<MaterialComponent>(groundEnt,
            MaterialComponent{ defaultTex, flatNorm, /*shininess*/0.0f, /*metallic*/0.0f, /*roughness*/0.9f, /*emissive*/0.0f });
        reg.emplace<MapComponent>(groundEnt);
        auto& gt   = reg.get<TransformComponent>(groundEnt);
        gt.position = glm::vec3(100.0f, -0.01f, 100.0f); // centred, just under lane tiles
        gt.scale    = glm::vec3(1.0f);
    }

    // ── Load map models from "map models/" ─────────────────────────────
    struct MapAsset {
        uint32_t mesh;
        uint32_t texture;                    // primary (sub-mesh 0) texture
        std::vector<uint32_t> subMeshTextures; // per-sub-mesh textures (empty = all use 'texture')
    };
    std::unordered_map<std::string, MapAsset> mapAssets;

    auto loadMapAsset = [&](const std::string& filename) {
        std::string path = std::string(MODEL_DIR) + "map models/" + filename;
        try {
            Model model = Model::loadFromGLB(*m_device, m_device->getAllocator(), path);

            // Read per-submesh glTF material indices BEFORE moving model into scene
            uint32_t subCount = model.getMeshCount();
            std::vector<int> subMatIndices;
            subMatIndices.reserve(subCount);
            for (uint32_t si = 0; si < subCount; ++si)
                subMatIndices.push_back(model.getMeshMaterialIndex(si));

            uint32_t meshIdx = m_scene.addMesh(std::move(model));

            // Load ALL embedded textures and build glTF-materialIndex → scene-texIdx map
            auto glbTextures = Model::loadGLBTextures(*m_device, path);
            std::unordered_map<int, uint32_t> matToTex;
            uint32_t primaryTex = defaultTex;
            for (auto& t : glbTextures) {
                uint32_t tid = m_scene.addTexture(std::move(t.texture));
                m_bindless->registerTexture(
                    m_scene.getTexture(tid).getImageView(),
                    m_scene.getTexture(tid).getSampler());
                matToTex[t.materialIndex] = tid;
                if (primaryTex == defaultTex) primaryTex = tid;
            }

            // Map each sub-mesh to its correct texture
            std::vector<uint32_t> subTextures;
            subTextures.reserve(subCount);
            for (int gltfMatIdx : subMatIndices)
                subTextures.push_back(matToTex.count(gltfMatIdx) ? matToTex[gltfMatIdx] : primaryTex);

            mapAssets[filename] = { meshIdx, primaryTex, subTextures };
            spdlog::info("Loaded map asset: {} (mesh={}, {} textures, {} submeshes)",
                         filename, meshIdx, glbTextures.size(), subCount);
        } catch (const std::exception& e) {
            spdlog::warn("Failed to load map asset '{}': {}", filename, e.what());
        }
    };

    loadMapAsset("blue_team_tower_1.glb");
    loadMapAsset("blue_team_tower_2.glb");
    loadMapAsset("blue_team_tower_3.glb");
    loadMapAsset("blue_team_inhib.glb");
    loadMapAsset("blue_team_nexus.glb");
    loadMapAsset("red_team_tower_1.glb");
    loadMapAsset("read_team_tower_2.glb");
    loadMapAsset("red_team_tower3.glb");
    loadMapAsset("red_team_inhib.glb");
    loadMapAsset("red_team_nexus.glb");
    loadMapAsset("arcane+tile+3d+model.glb");
    loadMapAsset("jungle_tile.glb");
    loadMapAsset("river_tile.glb");

    // ── Give structures proper meshes from map models ───────────────────
    {
        auto& reg = m_scene.getRegistry();
        auto structView = reg.view<StructureComponent, TransformComponent>();
        for (auto e : structView) {
            if (reg.all_of<MeshComponent>(e)) continue;
            auto& sc = structView.get<StructureComponent>(e);
            auto& tc = structView.get<TransformComponent>(e);

            std::string modelFile;
            if (sc.teamIndex == 0) { // Blue
                switch (sc.type) {
                    case StructureType::TOWER_T1:    modelFile = "blue_team_tower_1.glb"; break;
                    case StructureType::TOWER_T2:    modelFile = "blue_team_tower_2.glb"; break;
                    case StructureType::TOWER_T3:    modelFile = "blue_team_tower_3.glb"; break;
                    case StructureType::TOWER_NEXUS: modelFile = "blue_team_tower_3.glb"; break;
                    case StructureType::INHIBITOR:   modelFile = "blue_team_inhib.glb";   break;
                    case StructureType::NEXUS:       modelFile = "blue_team_nexus.glb";   break;
                }
            } else { // Red
                switch (sc.type) {
                    case StructureType::TOWER_T1:    modelFile = "red_team_tower_1.glb";  break;
                    case StructureType::TOWER_T2:    modelFile = "read_team_tower_2.glb"; break;
                    case StructureType::TOWER_T3:    modelFile = "red_team_tower3.glb";   break;
                    case StructureType::TOWER_NEXUS: modelFile = "red_team_tower3.glb";   break;
                    case StructureType::INHIBITOR:   modelFile = "red_team_inhib.glb";    break;
                    case StructureType::NEXUS:       modelFile = "red_team_nexus.glb";    break;
                }
            }

            if (mapAssets.count(modelFile)) {
                reg.emplace<MeshComponent>(e, MeshComponent{ mapAssets[modelFile].mesh });
                reg.emplace<MaterialComponent>(e, MaterialComponent{
                    mapAssets[modelFile].texture, flatNorm,
                    /*shininess=*/0.0f, /*metallic=*/0.0f,
                    /*roughness=*/0.6f, /*emissive=*/0.0f,
                    mapAssets[modelFile].subMeshTextures });
                tc.scale = glm::vec3(1.0f); 
            } else {
                // Fallback to cube if model failed to load
                uint32_t cubeMesh = m_scene.addMesh(Model::createCube(*m_device, m_device->getAllocator()));
                float s = 1.0f;
                switch (sc.type) {
                    case StructureType::TOWER_T1:
                    case StructureType::TOWER_T2:
                    case StructureType::TOWER_T3:
                    case StructureType::TOWER_NEXUS: s = 2.0f;  break;
                    case StructureType::INHIBITOR:   s = 2.5f;  break;
                    case StructureType::NEXUS:       s = 3.5f;  break;
                }
                tc.scale = glm::vec3(s, s * 2.0f, s);
                reg.emplace<MeshComponent>(e, MeshComponent{ cubeMesh });
                reg.emplace<MaterialComponent>(e,
                    MaterialComponent{ defaultTex, flatNorm, 0.0f, 0.0f, 0.3f, 0.6f });
            }
        }
        spdlog::info("[Renderer] Structure meshes assigned from map models");
    }

    // ── Lane tiles (stone-textured flat quads along each lane path) ─────────
    {
        std::string laneTilePath = std::string(MODEL_DIR) +
                                   "map models/arcane+tile+3d+model.glb";
        uint32_t laneTileMesh = 0;
        uint32_t laneTileTex  = checkerTex;
        bool     laneTileOK   = false;

        // Procedural 1×1 flat quad — fast (2 tris), reused for all 86 tiles.
        auto tileQuad = Model::createTerrain(*m_device, m_device->getAllocator(),
                                             1.0f, 1, 0.0f);
        laneTileMesh = m_scene.addMesh(std::move(tileQuad));

        if (mapAssets.count("arcane+tile+3d+model.glb")) {
            laneTileTex = mapAssets["arcane+tile+3d+model.glb"].texture;
            laneTileOK = true;
        }

        if (laneTileOK) {
            // Quad is 1×1; scale it so it covers the lane width in both axes.
            auto placeLaneTiles = [&](const std::vector<glm::vec3>& waypoints,
                                      float laneWidth, const std::string& laneName,
                                      float laneY)
            {
                int tileCount = 0;
                float stride = laneWidth;

                for (size_t i = 0; i + 1 < waypoints.size(); ++i) {
                    glm::vec3 a    = waypoints[i];
                    glm::vec3 b    = waypoints[i + 1];
                    glm::vec3 diff = b - a;
                    float segLen   = glm::length(diff);
                    if (segLen < 0.001f) continue;
                    glm::vec3 dir = diff / segLen;

                    float yaw = std::atan2(dir.x, dir.z);

                    float walked = 0.0f;
                    while (walked < segLen) {
                        glm::vec3 pos = a + dir * (walked + stride * 0.5f);
                        pos.y = laneY;

                        uint32_t currentMesh = laneTileMesh;
                        uint32_t currentTex  = laneTileTex;

                        // River / jungle zones: swap texture only (keep flat quad mesh)
                        if (std::abs(pos.x + pos.z - 200.0f) < 25.0f) {
                            if (mapAssets.count("river_tile.glb"))
                                currentTex = mapAssets["river_tile.glb"].texture;
                        } else if (laneName != "MidLane") {
                            if (mapAssets.count("jungle_tile.glb"))
                                currentTex = mapAssets["jungle_tile.glb"].texture;
                        }

                        auto tile = m_scene.createEntity(
                            laneName + "_tile_" + std::to_string(tileCount));
                        m_scene.getRegistry().emplace<MeshComponent>(
                            tile, MeshComponent{ currentMesh });
                        m_scene.getRegistry().emplace<MaterialComponent>(
                            tile, MaterialComponent{ currentTex, flatNorm,
                                                     /*shininess*/0.0f, /*metallic*/0.0f,
                                                     /*roughness*/0.8f, /*emissive*/0.0f });
                        m_scene.getRegistry().emplace<MapComponent>(tile);

                        auto& tt    = m_scene.getRegistry().get<TransformComponent>(tile);
                        tt.position = pos;
                        tt.rotation = glm::vec3(0.0f, yaw, 0.0f);
                        
                        tt.scale = glm::vec3(laneWidth);

                        ++tileCount;
                        walked += stride;
                    }
                }
                spdlog::info("Placed {} tiles for {}", tileCount, laneName);
            };

            // Use actual lane waypoints from map data rather than hardcoded paths.
            // Stagger Y per lane so overlapping tiles don't Z-fight.  Mid lane on
            // top (widest, most visible), Top/Bot underneath at distinct heights.
            const auto& blueTeam = m_mapData.teams[0];
            if (!blueTeam.lanes[0].waypoints.empty()) {
                placeLaneTiles(blueTeam.lanes[0].waypoints,
                               blueTeam.lanes[0].width, "TopLane", 0.05f);
            } else {
                placeLaneTiles({
                    {22,0,22}, {22,0,60}, {22,0,100}, {22,0,140}, {22,0,178},
                    {60,0,178}, {100,0,178}, {140,0,178}, {178,0,178}
                }, 12.0f, "TopLane", 0.05f);
            }

            // Mid Lane
            if (!blueTeam.lanes[1].waypoints.empty()) {
                placeLaneTiles(blueTeam.lanes[1].waypoints,
                               blueTeam.lanes[1].width, "MidLane", 0.15f);
            } else {
                placeLaneTiles({
                    {22,0,22}, {40,0,40}, {60,0,60}, {80,0,80}, {100,0,100},
                    {120,0,120}, {140,0,140}, {160,0,160}, {178,0,178}
                }, 14.0f, "MidLane", 0.15f);
            }

            // Bot Lane
            if (!blueTeam.lanes[2].waypoints.empty()) {
                placeLaneTiles(blueTeam.lanes[2].waypoints,
                               blueTeam.lanes[2].width, "BotLane", 0.10f);
            } else {
                placeLaneTiles({
                    {22,0,22}, {60,0,22}, {100,0,22}, {140,0,22}, {178,0,22},
                    {178,0,60}, {178,0,100}, {178,0,140}, {178,0,178}
                }, 12.0f, "BotLane", 0.10f);
            }
        }
    }

    // ── Player character ─────────────────────────────────────────────────
    auto character = m_scene.createEntity("PlayerCharacter");
    bool skinnedLoaded = false;

    try {
        std::string charPath = std::string(MODEL_DIR) + "models/scientist/scientist.glb";
        auto skinnedData = Model::loadSkinnedFromGLB(
            *m_device, m_device->getAllocator(), charPath, 0.0f);

        // Load ALL textures and build per-sub-mesh mapping
        uint32_t charTex = defaultTex;
        std::vector<uint32_t> charSubMeshTextures;
        auto glbTextures = Model::loadGLBTextures(*m_device, charPath);
        if (!glbTextures.empty()) {
            std::unordered_map<int, uint32_t> matToTex;
            for (auto& t : glbTextures) {
                uint32_t tid = m_scene.addTexture(std::move(t.texture));
                m_bindless->registerTexture(
                    m_scene.getTexture(tid).getImageView(),
                    m_scene.getTexture(tid).getSampler());
                matToTex[t.materialIndex] = tid;
                if (charTex == defaultTex) charTex = tid;
            }
            spdlog::info("Character: {} texture(s) loaded, primary slot {}", glbTextures.size(), charTex);
        } else {
            spdlog::warn("Character texture not found in GLB — using default white texture (slot {})", charTex);
        }

        m_impostorSystem.registerUnitType("hero", &m_scene.getMesh(m_scene.addMesh(std::move(skinnedData.model))), 1.5f, 2.0f, 16);

        // Build StaticSkinnedMesh from first mesh in GLB
        if (!skinnedData.bindPoseVertices.empty() && !skinnedData.skinVertices.empty()) {
            std::vector<SkinnedVertex> sverts;
            sverts.reserve(skinnedData.bindPoseVertices[0].size());
            for (size_t vi = 0; vi < skinnedData.bindPoseVertices[0].size(); ++vi) {
                SkinnedVertex sv{};
                sv.position = skinnedData.bindPoseVertices[0][vi].position;
                sv.color    = skinnedData.bindPoseVertices[0][vi].color;
                sv.normal   = skinnedData.bindPoseVertices[0][vi].normal;
                sv.texCoord = skinnedData.bindPoseVertices[0][vi].texCoord;
                sv.joints   = skinnedData.skinVertices[0][vi].joints;
                sv.weights  = skinnedData.skinVertices[0][vi].weights;
                sverts.push_back(sv);
            }
            uint32_t ssIdx = m_scene.addStaticSkinnedMesh(
                StaticSkinnedMesh(*m_device, m_device->getAllocator(),
                                  sverts, skinnedData.indices[0]));

            // Setup animation
            SkeletonComponent skelComp;
            skelComp.skeleton         = std::move(skinnedData.skeleton);
            skelComp.skinVertices     = std::move(skinnedData.skinVertices);
            skelComp.bindPoseVertices = std::move(skinnedData.bindPoseVertices);

            AnimationComponent animComp;
            animComp.player.setSkeleton(&skelComp.skeleton);

            // Try to load idle and walk animations
            auto tryLoadAnim = [&](const std::string& path) {
                try {
                    auto d = Model::loadSkinnedFromGLB(*m_device, m_device->getAllocator(), path, 0.0f);
                    if (!d.animations.empty()) {
                        animComp.clips.push_back(std::move(d.animations[0]));
                        retargetClip(animComp.clips.back(), skelComp.skeleton);
                        return true;
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to load animation '{}': {}", path, e.what());
                }
                return false;
            };

            std::string base = std::string(MODEL_DIR) + "models/scientist/";
            bool idleOk = tryLoadAnim(base + "scientist_idle.glb");
            if (!idleOk && !skinnedData.animations.empty())
                animComp.clips.push_back(skinnedData.animations[0]); // embedded fallback
            tryLoadAnim(base + "scientist_walk.glb");
            // Attack animation (clip[2]): play once and hold — no looping
            if (tryLoadAnim(base + "scientist_auto_attack.glb"))
                animComp.clips.back().looping = false;

            if (!animComp.clips.empty()) {
                animComp.activeClipIndex = 0;
                animComp.player.setClip(&animComp.clips[0]);
            }

            m_scene.getRegistry().emplace<SkeletonComponent>(character, std::move(skelComp));
            m_scene.getRegistry().emplace<AnimationComponent>(character, std::move(animComp));

            // Re-point raw pointers to the registry-owned copies (the locals were moved-from)
            auto& regSkel = m_scene.getRegistry().get<SkeletonComponent>(character);
            auto& regAnim = m_scene.getRegistry().get<AnimationComponent>(character);
            regAnim.player.setSkeleton(&regSkel.skeleton);
            if (!regAnim.clips.empty())
                regAnim.player.setClip(&regAnim.clips[regAnim.activeClipIndex]);

            m_scene.getRegistry().emplace<GPUSkinnedMeshComponent>(character,
                GPUSkinnedMeshComponent{ ssIdx });
            m_scene.getRegistry().emplace<MaterialComponent>(character,
                MaterialComponent{ charTex, flatNorm, 0.0f, 0.0f, 0.5f, 0.0f });

            auto& ct = m_scene.getRegistry().get<TransformComponent>(character);
            ct.position  = glm::vec3(100.0f, 0.0f, 100.0f);
            ct.scale     = glm::vec3(0.05f);
            skinnedLoaded = true;
            spdlog::info("Character loaded with GPU skinning");
        }
    } catch (const std::exception& e) {
        spdlog::warn("Could not load skinned character: {} — using capsule fallback", e.what());
    }

    if (!skinnedLoaded) {
        // Fallback: simple capsule
        uint32_t capsuleMesh = m_scene.addMesh(
            Model::createCapsule(*m_device, m_device->getAllocator(), 0.5f, 1.0f));
        m_scene.getRegistry().emplace<MeshComponent>(character, MeshComponent{ capsuleMesh });
        m_scene.getRegistry().emplace<MaterialComponent>(character,
            MaterialComponent{ defaultTex, flatNorm, 0.0f, 0.0f, 0.5f, 0.0f });
        auto& ct = m_scene.getRegistry().get<TransformComponent>(character);
        ct.position = glm::vec3(100.0f, 0.0f, 100.0f);
        ct.scale    = glm::vec3(1.0f);
    }

    // Look up selected hero definition
    const HeroDefinition* heroDef = m_heroRegistry.find(m_selectedHeroId);
    if (!heroDef && m_heroRegistry.count() > 0)
        heroDef = &m_heroRegistry.all()[0];

    float moveSpeed = heroDef ? heroDef->baseMoveSpeed : 8.0f;
    m_scene.getRegistry().emplace<CharacterComponent>(character,
        CharacterComponent{ glm::vec3(100.0f, 0.0f, 100.0f), moveSpeed });
    m_scene.getRegistry().emplace<TeamComponent>(character, TeamComponent{ Team::PLAYER });
    auto& combat = m_scene.getRegistry().emplace<CombatComponent>(character);
    if (heroDef) {
        combat.isRanged = heroDef->isRanged;
        combat.projectileSpeed = heroDef->projectileSpeed;
        combat.projectileVfx = heroDef->projectileVfx;
        combat.attackRange = heroDef->baseAttackRange;
        combat.attackSpeed = heroDef->baseAttackSpeed;
        combat.attackDamage = heroDef->baseAttackDamage;
    } else {
        combat.isRanged = true;
        combat.projectileSpeed = 30.0f;
        combat.projectileVfx = "vfx_fireball_projectile";
        combat.attackRange = 15.0f;
        combat.attackSpeed = 1.2f;
    }

    m_scene.getRegistry().emplace<SelectableComponent>(character, SelectableComponent{ false, 2.5f });

    ResourceComponent res;
    if (heroDef) {
        res.maximum = heroDef->baseMP;
        res.current = heroDef->baseMP;
    }
    m_scene.getRegistry().emplace<ResourceComponent>(character, res);
    
    StatsComponent playerStats;
    if (heroDef) {
        playerStats.base.maxHP = heroDef->baseHP;
        playerStats.base.currentHP = heroDef->baseHP;
        playerStats.base.attackDamage = heroDef->baseAttackDamage;
        playerStats.base.armor = heroDef->baseArmor;
        playerStats.base.magicResist = heroDef->baseMagicResist;
        playerStats.base.abilityPower = heroDef->baseAbilityPower;
    } else {
        playerStats.base.maxHP = 600.0f;
        playerStats.base.currentHP = 600.0f;
    }
    m_scene.getRegistry().emplace<StatsComponent>(character, playerStats);

    m_scene.getRegistry().emplace<StatusEffectsComponent>(character);

    HeroComponent heroComp;
    heroComp.definition = heroDef;
    heroComp.level = 1;
    m_scene.getRegistry().emplace<HeroComponent>(character, heroComp);

    // Economy: starting gold + XP tracking
    m_scene.getRegistry().emplace<EconomyComponent>(character, EconomyComponent{500, 0, 1});

    // Respawn: hero can die and respawn
    m_scene.getRegistry().emplace<RespawnComponent>(character, RespawnComponent{
        LifeState::ALIVE, 0.f, 0.f, glm::vec3(0.f), true /*isHero*/
    });

    // FoW vision: hero sight radius
    m_scene.getRegistry().emplace<VisionComponent>(character, VisionComponent{
        FogOfWarGameplay::HERO_VISION
    });
    if (m_abilitySystem) {
        std::array<std::string, 4> abilityIds;
        if (heroDef) {
            abilityIds = heroDef->abilityIds;
        }
        bool hasAbilities = false;
        for (auto& id : abilityIds) {
            if (!id.empty()) { hasAbilities = true; break; }
        }
        if (!hasAbilities) {
            abilityIds = {"fire_mage_fireball", "ice_zone", "nature_shield", "storm_strike"};
        }
        m_abilitySystem->initEntity(m_scene.getRegistry(), character, abilityIds);
        m_abilitySystem->setAbilityLevel(m_scene.getRegistry(), character, AbilitySlot::Q, 1);
        m_abilitySystem->setAbilityLevel(m_scene.getRegistry(), character, AbilitySlot::W, 1);
        m_abilitySystem->setAbilityLevel(m_scene.getRegistry(), character, AbilitySlot::E, 1);
        m_abilitySystem->setAbilityLevel(m_scene.getRegistry(), character, AbilitySlot::R, 1);

        // D-key summoner ability
        std::string summonerId = heroDef ? heroDef->summonerAbilityId : "fire_mage_trick";
        if (!summonerId.empty()) {
            const auto* trickDef = m_abilitySystem->findDefinition(summonerId);
            if (trickDef) {
                auto& book = m_scene.getRegistry().get<AbilityBookComponent>(character);
                auto& inst = book.abilities[static_cast<size_t>(AbilitySlot::SUMMONER)];
                inst.def   = trickDef;
                inst.level = 1;
                inst.currentPhase = AbilityPhase::READY;
            }
        }
    }
    
    m_playerEntity = character;
    m_gameplaySystem.init(m_scene, m_abilitySystem.get(), m_combatSystem.get(),
                          &m_gpuCollision, &m_debugRenderer);
    m_gameplaySystem.setGroundDecals(m_groundDecalRenderer.get());
    m_gameplaySystem.setPlayerEntity(m_playerEntity);
    m_isoCam.setFollowTarget(glm::vec3(100.0f, 0.0f, 100.0f));

    // ── Load Minion assets for spawning ───────────────────────────────────
    try {
        std::string minionPath = std::string(MODEL_DIR) + "models/melee_minion/melee_minion_walking.glb";
        auto skinnedData = Model::loadSkinnedFromGLB(*m_device, m_device->getAllocator(), minionPath, 0.0f);

        uint32_t minionTex = defaultTex;
        auto glbTextures = Model::loadGLBTextures(*m_device, minionPath);
        if (!glbTextures.empty()) {
            for (auto& t : glbTextures) {
                uint32_t tid = m_scene.addTexture(std::move(t.texture));
                m_bindless->registerTexture(
                    m_scene.getTexture(tid).getImageView(),
                    m_scene.getTexture(tid).getSampler());
                if (minionTex == defaultTex) minionTex = tid;
            }
            spdlog::info("Minion: {} texture(s) loaded, primary slot {}", glbTextures.size(), minionTex);
        } else {
            spdlog::warn("Minion texture not found in GLB — using default white texture (slot {})", minionTex);
        }

        m_impostorSystem.registerUnitType("minion", &m_scene.getMesh(m_scene.addMesh(std::move(skinnedData.model))), 1.0f, 1.2f, 16);
        m_impostorSystem.registerUnitType("default", &m_scene.getMesh(m_scene.getMeshes().size() - 1), 1.0f, 1.2f, 16);

        if (!skinnedData.bindPoseVertices.empty() && !skinnedData.skinVertices.empty()) {
            std::vector<SkinnedVertex> sverts;
            sverts.reserve(skinnedData.bindPoseVertices[0].size());
            for (size_t vi = 0; vi < skinnedData.bindPoseVertices[0].size(); ++vi) {
                SkinnedVertex sv{};
                sv.position = skinnedData.bindPoseVertices[0][vi].position;
                sv.color    = skinnedData.bindPoseVertices[0][vi].color;
                sv.normal   = skinnedData.bindPoseVertices[0][vi].normal;
                sv.texCoord = skinnedData.bindPoseVertices[0][vi].texCoord;
                sv.joints   = skinnedData.skinVertices[0][vi].joints;
                sv.weights  = skinnedData.skinVertices[0][vi].weights;
                sverts.push_back(sv);
            }
            uint32_t minionMeshIdx = m_scene.addStaticSkinnedMesh(
                StaticSkinnedMesh(*m_device, m_device->getAllocator(),
                                  sverts, skinnedData.indices[0]));

            Skeleton minionSkeleton          = std::move(skinnedData.skeleton);
            auto     minionSkinVertices      = std::move(skinnedData.skinVertices);
            auto     minionBindPoseVertices  = std::move(skinnedData.bindPoseVertices);
            std::vector<AnimationClip> minionClips;

            // Load idle animation from melee_minion_idle.glb (clip[0])
            // Falls back to empty clip if file not found
            std::string minionAnimBase = std::string(MODEL_DIR) + "models/melee_minion/";
            bool idleOk = false;
            try {
                auto idleData = Model::loadSkinnedFromGLB(
                    *m_device, m_device->getAllocator(),
                    minionAnimBase + "melee_minion_idle.glb", 0.0f);
                if (!idleData.animations.empty()) {
                    minionClips.push_back(std::move(idleData.animations[0]));
                    retargetClip(minionClips.back(), minionSkeleton);
                    idleOk = true;
                    spdlog::info("Minion idle animation loaded and retargeted");
                }
            } catch (const std::exception& ie) {
                spdlog::warn("Could not load minion idle animation: {}", ie.what());
            }
            if (!idleOk)
                minionClips.push_back(AnimationClip{}); // empty fallback

            // Walk animation is embedded in the base model (clip[1])
            if (!skinnedData.animations.empty()) {
                minionClips.push_back(std::move(skinnedData.animations[0]));
            } else {
                spdlog::warn("No walk animation found in minion base model.");
            }

            // Attack animation (clip[2])
            bool attackOk = false;
            try {
                auto attackData = Model::loadSkinnedFromGLB(
                    *m_device, m_device->getAllocator(),
                    minionAnimBase + "melee_minion_attack1.glb", 0.0f);
                if (!attackData.animations.empty()) {
                    minionClips.push_back(std::move(attackData.animations[0]));
                    retargetClip(minionClips.back(), minionSkeleton);
                    minionClips.back().looping = false; // Attack plays once, don't loop
                    attackOk = true;
                    spdlog::info("Minion attack animation loaded and retargeted");
                }
            } catch (const std::exception& ae) {
                spdlog::warn("Could not load minion attack animation: {}", ae.what());
            }
            if (!attackOk)
                minionClips.push_back(AnimationClip{}); // empty fallback

            spdlog::info("Minion model and {} animations loaded for spawning", minionClips.size());

            MinionSpawnConfig spawnCfg;
            spawnCfg.meshIndex        = minionMeshIdx;
            spawnCfg.texIndex         = minionTex;
            spawnCfg.flatNormIndex    = flatNorm;
            spawnCfg.skeleton         = std::move(minionSkeleton);
            spawnCfg.skinVertices     = std::move(minionSkinVertices);
            spawnCfg.bindPoseVertices = std::move(minionBindPoseVertices);
            spawnCfg.clips            = std::move(minionClips);
            m_gameplaySystem.setSpawnConfig(std::move(spawnCfg));

            // Copy spawn config to MinionWaveSystem for lane wave spawning
            {
                const auto& src = m_gameplaySystem.getSpawnConfig();
                WaveSpawnConfig wsc;
                wsc.meshIndex     = src.meshIndex;
                wsc.texIndex      = src.texIndex;
                wsc.flatNormIndex = src.flatNormIndex;
                wsc.skeleton      = src.skeleton;
                wsc.skinVertices  = src.skinVertices;
                wsc.bindPoseVertices = src.bindPoseVertices;
                wsc.clips         = src.clips;
                wsc.ready         = true;
                m_minionWaveSystem->setSpawnConfig(std::move(wsc));
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("Could not load minion model: {}", e.what());
    }

    spdlog::info("Scene built: flat map + player character");

    // ── Pre-load Q ability model (q+ability.glb) for projectile rendering ────
    if (m_projectileSystem) {
        try {
            std::string qModelPath = std::string(MODEL_DIR) + "models/abiliities_models/q+ability.glb";
            auto model = Model::loadFromGLB(*m_device, m_device->getAllocator(), qModelPath);
            uint32_t qMeshIdx = m_scene.addMesh(std::move(model));

            // Try to load embedded texture(s); fall back to default
            uint32_t qTexIdx = defaultTex;
            auto glbTextures = Model::loadGLBTextures(*m_device, qModelPath);
            if (!glbTextures.empty()) {
                for (auto& t : glbTextures) {
                    uint32_t tid = m_scene.addTexture(std::move(t.texture));
                    m_bindless->registerTexture(
                        m_scene.getTexture(tid).getImageView(),
                        m_scene.getTexture(tid).getSampler());
                    if (qTexIdx == defaultTex) qTexIdx = tid;
                }
            }

            // Register with the projectile system (scale = 1.0 — model is ~1 unit long)
            m_projectileSystem->registerAbilityMesh("fire_mage_fireball",
                { qMeshIdx, qTexIdx, m_flatNormIndex, glm::vec3(3.5f) });

            // Reuse the same Q model for the D-key purple trick skillshot
            // but with a solid purple texture so it's visually distinct
            uint32_t purplePixel = 0xFF9900CC;  // ABGR: opaque purple (R=0xCC, G=0x00, B=0x99)
            auto purpleTex = Texture::createFromPixels(*m_device, &purplePixel, 1, 1);
            uint32_t purpleTexIdx = m_scene.addTexture(std::move(purpleTex));
            m_bindless->registerTexture(
                m_scene.getTexture(purpleTexIdx).getImageView(),
                m_scene.getTexture(purpleTexIdx).getSampler());
            m_projectileSystem->registerAbilityMesh("fire_mage_trick",
                { qMeshIdx, purpleTexIdx, m_flatNormIndex, glm::vec3(3.0f) });

            // Reuse Q model for R-key bomb lob with a bright red-orange texture
            uint32_t bombPixel = 0xFF0044FF;  // ABGR: opaque red-orange
            auto bombTex = Texture::createFromPixels(*m_device, &bombPixel, 1, 1);
            uint32_t bombTexIdx = m_scene.addTexture(std::move(bombTex));
            m_bindless->registerTexture(
                m_scene.getTexture(bombTexIdx).getImageView(),
                m_scene.getTexture(bombTexIdx).getSampler());
            m_projectileSystem->registerAbilityMesh("fire_mage_bomb",
                { qMeshIdx, bombTexIdx, m_flatNormIndex, glm::vec3(4.5f) });

            spdlog::info("Q ability model loaded (meshIdx={})", qMeshIdx);
        } catch (const std::exception& e) {
            spdlog::warn("Could not load Q ability model: {}", e.what());
        }
    }

    // ── Water renderer ────────────────────────────────────────────────────
    // Register its 3 procedural textures just after all scene textures.
    {
        m_waterRenderer = std::make_unique<WaterRenderer>();
        m_waterRenderer->init(*m_device, m_hdrFB->mainFormats(),
                              m_descriptors->getLayout(),
                              m_bindless->getLayout(),
                              *m_bindless);
    }

    // ── Mega-buffer: suballocate all scene meshes ────────────────────────
    if (m_megaBuffer) {
        const auto& models = m_scene.getMeshes();
        m_meshHandles.resize(models.size());

        for (uint32_t mi = 0; mi < static_cast<uint32_t>(models.size()); ++mi) {
            auto& model = m_scene.getMesh(mi);
            uint32_t subCount = model.getMeshCount();
            m_meshHandles[mi].resize(subCount);

            for (uint32_t si = 0; si < subCount; ++si) {
                auto& mesh = model.getSubMesh(si);
                const auto& verts = mesh.getCPUVertices();
                const auto& idxs  = mesh.getCPUIndices();
                if (!verts.empty() && !idxs.empty()) {
                    m_meshHandles[mi][si] = m_megaBuffer->suballocate(
                        verts.data(), static_cast<uint32_t>(verts.size()),
                        idxs.data(),  static_cast<uint32_t>(idxs.size()));
                }
                mesh.releaseCPUData();
            }
        }

        // One-shot transfer staging → device-local
        m_megaBuffer->flush();
        spdlog::info("Mega-buffer: {} models suballocated",
                     models.size());
    }

    // ── LOD system: load config + register chains from cooked LOD data ───
    {
        m_lodSystem.loadConfig(std::string(ASSET_DIR) + "config/render_settings.json");
        m_lodSystem.setQuality(static_cast<int>(m_renderQuality));
        spdlog::info("[Renderer] LODSystem initialised (quality={})",
                     static_cast<int>(m_renderQuality));
    }

    // ── Impostor system ──────────────────────────────────────────────────
    {
        RenderFormats impostorFmts = m_hdrFB->mainFormats();
        m_impostorSystem.init(*m_device, impostorFmts);
        
        // Register default unit types for impostor capturing
        // Note: we use pointers to models stored in m_scene to ensure they stay alive
        if (m_scene.getMeshes().size() > 0) {
            // Heuristic: register some common models if they were loaded
            // (The individual loading blocks already registered "hero" and "minion")
            m_impostorSystem.generateAtlas();
        }
    }
}

// ── spawnTestEnemy ───────────────────────────────────────────────────────────
void Renderer::spawnTestEnemy() {
    if (m_playerEntity == entt::null) return;

    auto& reg = m_scene.getRegistry();
    glm::vec3 playerPos = reg.get<TransformComponent>(m_playerEntity).position;
    glm::vec3 spawnPos = playerPos + glm::vec3(5.0f, 0.0f, 0.0f);

    auto enemy = m_scene.createEntity("TestDummy");
    auto& t = reg.get<TransformComponent>(enemy);
    t.position = spawnPos;
    t.scale    = glm::vec3(0.05f);

    reg.emplace<SelectableComponent>(enemy, SelectableComponent{ false, 2.5f });
    reg.emplace<UnitComponent>(enemy, UnitComponent{ UnitComponent::State::IDLE, spawnPos, 0.0f });
    // moveSpeed=0 → immobile test dummy
    reg.emplace<CharacterComponent>(enemy, CharacterComponent{ spawnPos, 0.0f });
    reg.emplace<TeamComponent>(enemy, TeamComponent{ Team::ENEMY });
    reg.emplace<TestDummyTag>(enemy);
    reg.emplace<CombatComponent>(enemy);

    // StatsComponent with explicit values
    StatsComponent stats;
    stats.base.maxHP      = 600.0f;
    stats.base.currentHP  = 600.0f;
    stats.base.armor      = 30.0f;
    reg.emplace<StatsComponent>(enemy, stats);

    // Respawn: test enemies are non-hero (destroyed on death)
    reg.emplace<RespawnComponent>(enemy, RespawnComponent{
        .state = LifeState::ALIVE, .isHero = false
    });
    const auto& spawnCfg = m_gameplaySystem.getSpawnConfig();
    reg.emplace<GPUSkinnedMeshComponent>(enemy, GPUSkinnedMeshComponent{ spawnCfg.meshIndex });
    uint32_t flatNorm = 0;
    reg.emplace<MaterialComponent>(enemy,
        MaterialComponent{ spawnCfg.texIndex, flatNorm, 0.0f, 0.0f, 0.5f, 0.2f });

    SkeletonComponent skelComp;
    skelComp.skeleton         = spawnCfg.skeleton;
    skelComp.skinVertices     = spawnCfg.skinVertices;
    skelComp.bindPoseVertices = spawnCfg.bindPoseVertices;

    AnimationComponent animComp;
    animComp.player.setSkeleton(&skelComp.skeleton);
    animComp.clips = spawnCfg.clips;
    if (!animComp.clips.empty()) {
        animComp.activeClipIndex = 0; // idle
        animComp.player.setClip(&animComp.clips[0]);
    }
    reg.emplace<SkeletonComponent>(enemy, std::move(skelComp));
    reg.emplace<AnimationComponent>(enemy, std::move(animComp));

    // Re-point raw pointers to registry-owned copies
    auto& regSkel = reg.get<SkeletonComponent>(enemy);
    auto& regAnim = reg.get<AnimationComponent>(enemy);
    regAnim.player.setSkeleton(&regSkel.skeleton);
    if (!regAnim.clips.empty())
        regAnim.player.setClip(&regAnim.clips[regAnim.activeClipIndex]);

    spdlog::info("Spawned test dummy at ({:.1f}, {:.1f}, {:.1f})",
                 spawnPos.x, spawnPos.y, spawnPos.z);
}

// ── pickEntityUnderCursor ────────────────────────────────────────────────────
entt::entity Renderer::pickEntityUnderCursor() {
    glm::vec2 mousePos = m_input->getMousePos();
    glm::vec3 worldPos = screenToWorld(mousePos.x, mousePos.y);

    float closestDist = FLT_MAX;
    entt::entity closest = entt::null;

    auto view = m_scene.getRegistry().view<TransformComponent, TeamComponent, SelectableComponent>();
    for (auto [entity, transform, team, selectable] : view.each()) {
        if (team.team != Team::ENEMY) continue;

        float dist = glm::distance(glm::vec2(worldPos.x, worldPos.z),
                                   glm::vec2(transform.position.x, transform.position.z));
        if (dist < selectable.selectionRadius && dist < closestDist) {
            closestDist = dist;
            closest = entity;
        }
    }
    return closest;
}

// ── recreateSwapchain ────────────────────────────────────────────────────────
void Renderer::recreateSwapchain() {
    VkExtent2D extent = m_window.getExtent();
    while (extent.width == 0 || extent.height == 0) {
        extent = m_window.getExtent();
        glfwWaitEvents();
    }
    vkDeviceWaitIdle(m_device->getDevice());
    m_swapchain->recreate(extent);
    m_hdrFB->recreate(extent.width, extent.height);
    m_bloom->recreate(m_hdrFB->colorView(), extent.width, extent.height);
    m_vfxRenderer->setDepthBuffer(m_hdrFB->depthView(), m_hdrFB->sampler());
    m_distortionRenderer->updateDescriptorSet(m_hdrFB->colorCopyView());
    if (m_inkingPass) m_inkingPass->updateInput(m_hdrFB->characterDepthView());
    m_toneMap->updateDescriptorSets(m_hdrFB->colorView(), m_bloom->bloomResultView());
    m_ssaoPass.recreate(extent.width, extent.height,
                        m_hdrFB->depthView(), m_hdrFB->sampler());
    m_ssrPass.recreate(extent.width, extent.height,
                       m_hdrFB->depthView(), m_hdrFB->sampler(),
                       m_hdrFB->colorView(), m_hdrFB->sampler(),
                       m_hizPass.getPyramidView(), m_hizPass.getSampler());
    m_sync->recreateRenderFinishedSemaphores(m_swapchain->getImageCount());
    spdlog::info("Swapchain recreated ({}×{})", extent.width, extent.height);
}

// ── screenToWorld ─────────────────────────────────────────────────────────────
// Unprojects a screen-space mouse position to a world-space point on the Y=0 plane.
glm::vec3 Renderer::screenToWorld(float mx, float my) const {
    auto ext  = m_swapchain->getExtent();
    float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);
    glm::mat4 vp = m_isoCam.getProjectionMatrix(aspect) * m_isoCam.getViewMatrix();

    int winW, winH;
    glfwGetWindowSize(m_window.getHandle(), &winW, &winH);

    // NDC
    glm::vec4 ndc{
        (mx / static_cast<float>(winW))  * 2.0f - 1.0f,
        (my / static_cast<float>(winH)) * 2.0f - 1.0f,
        -1.0f, 1.0f
    };
    glm::vec4 rayClip{ ndc.x, ndc.y, -1.0f, 1.0f };
    glm::vec4 rayEye  = glm::inverse(m_isoCam.getProjectionMatrix(aspect)) * rayClip;
    rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0f, 0.0f);
    glm::vec3 rayWorld = glm::normalize(glm::vec3(
        glm::inverse(m_isoCam.getViewMatrix()) * rayEye));

    // Intersect with Y=0 plane: origin + t*dir = (x, 0, z) → t = -origin.y / dir.y
    glm::vec3 origin = m_isoCam.getPosition();
    if (std::abs(rayWorld.y) < 1e-5f) return origin;
    float t = -origin.y / rayWorld.y;
    return origin + t * rayWorld;
}

glm::vec2 Renderer::worldToScreen(const glm::vec3& worldPos) const {
    auto ext = m_swapchain->getExtent();
    float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);
    glm::mat4 vp = m_isoCam.getProjectionMatrix(aspect) * m_isoCam.getViewMatrix();
    
    int winW, winH;
    glfwGetWindowSize(m_window.getHandle(), &winW, &winH);
    
    glm::vec4 clipPos = vp * glm::vec4(worldPos, 1.0f);
    if (clipPos.w < 0.1f) return glm::vec2(-1000.0f); // Behind camera

    glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
    // Map ndc [-1, 1] to screen [0, w], [0, h]
    // Vulkan NDC Y is down (matches screen space)
    return glm::vec2(
        (ndc.x * 0.5f + 0.5f) * static_cast<float>(winW),
        (ndc.y * 0.5f + 0.5f) * static_cast<float>(winH)
    );
}

// ── Instance buffers ─────────────────────────────────────────────────────────
void Renderer::createInstanceBuffers() {
    m_instanceBuffers.clear();
    m_instanceMapped.clear();
    m_instanceBuffers.reserve(Sync::MAX_FRAMES_IN_FLIGHT);
    m_instanceMapped.reserve(Sync::MAX_FRAMES_IN_FLIGHT);

    VkDeviceSize size = MAX_INSTANCES * sizeof(InstanceData);
    for (uint32_t i = 0; i < Sync::MAX_FRAMES_IN_FLIGHT; ++i) {
        m_instanceBuffers.emplace_back(
            m_device->getAllocator(), size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        m_instanceMapped.push_back(m_instanceBuffers.back().map());
    }
}

void Renderer::destroyInstanceBuffers() {
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_instanceBuffers.size()); ++i) {
        if (m_instanceMapped[i]) {
            m_instanceBuffers[i].unmap();
            m_instanceMapped[i] = nullptr;
        }
    }
    m_instanceBuffers.clear();
    m_instanceMapped.clear();
}

// ── Grid pipeline ─────────────────────────────────────────────────────────────
void Renderer::createGridPipeline() {
    VkDevice dev = m_device->getDevice();

    auto readFile = [](const std::string& path) -> std::vector<char> {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("Shader not found: " + path);
        size_t sz = static_cast<size_t>(file.tellg());
        std::vector<char> buf(sz);
        file.seekg(0);
        file.read(buf.data(), static_cast<std::streamsize>(sz));
        return buf;
    };
    auto makeModule = [&](const std::vector<char>& code) {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = code.size();
        ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m), "shader module");
        return m;
    };

    VkShaderModule vert = makeModule(readFile(std::string(SHADER_DIR) + "grid.vert.spv"));
    VkShaderModule frag = makeModule(readFile(std::string(SHADER_DIR) + "grid.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vert, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main" };

    VkPipelineVertexInputStateCreateInfo   vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rast{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rast.polygonMode = VK_POLYGON_MODE_FILL; rast.lineWidth = 1.0f; rast.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE; ds.depthCompareOp = VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState gridBlends[2] = {blend, {}};
    cb.attachmentCount = 2; cb.pAttachments = gridBlends;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.size       = sizeof(glm::mat4) + sizeof(float);
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pc;
    VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &m_gridPipelineLayout), "grid layout");

    VkPipelineRenderingCreateInfo gridDynCI = m_pipeline->getRenderFormats().pipelineRenderingCI();

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.pNext               = &gridDynCI;
    pci.stageCount          = 2;
    pci.pStages             = stages;
    pci.pVertexInputState   = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState      = &vps;
    pci.pRasterizationState = &rast;
    pci.pMultisampleState   = &ms;
    pci.pDepthStencilState  = &ds;
    pci.pColorBlendState    = &cb;
    pci.pDynamicState       = &dyn;
    pci.layout              = m_gridPipelineLayout;
    pci.renderPass          = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_gridPipeline), "grid pipeline");

    vkDestroyShaderModule(dev, frag, nullptr);
    vkDestroyShaderModule(dev, vert, nullptr);
    spdlog::info("Grid pipeline created");
}

void Renderer::destroyGridPipeline() {
    VkDevice dev = m_device->getDevice();
    if (m_gridPipeline)       { vkDestroyPipeline(dev, m_gridPipeline, nullptr);       m_gridPipeline       = VK_NULL_HANDLE; }
    if (m_gridPipelineLayout) { vkDestroyPipelineLayout(dev, m_gridPipelineLayout, nullptr); m_gridPipelineLayout = VK_NULL_HANDLE; }
}

// ── GPU-skinned pipeline ──────────────────────────────────────────────────────
void Renderer::createSkinnedPipeline() {
    VkDevice dev = m_device->getDevice();

    auto readFile = [](const std::string& path) -> std::vector<char> {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("Shader not found: " + path);
        size_t sz = static_cast<size_t>(file.tellg());
        std::vector<char> buf(sz);
        file.seekg(0); file.read(buf.data(), static_cast<std::streamsize>(sz));
        return buf;
    };
    auto makeModule = [&](const std::vector<char>& code) {
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = code.size();
        ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m), "shader module");
        return m;
    };

    VkShaderModule vert = makeModule(readFile(std::string(SHADER_DIR) + "skinned.vert.spv"));
    VkShaderModule frag = makeModule(readFile(std::string(SHADER_DIR) + "triangle.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,   vert, "main" };
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main" };

    // SkinnedVertex (binding 0) + InstanceData (binding 1, locations shifted +2)
    auto skinnedBind  = SkinnedVertex::getBindingDescription();
    auto skinnedAttrs = SkinnedVertex::getAttributeDescriptions();
    auto instBind     = InstanceData::getBindingDescription();
    auto instAttrs    = InstanceData::getAttributeDescriptions();
    instBind.binding = 1;
    for (auto& a : instAttrs) { a.binding = 1; a.location += 2; } // 4→6, …, 14→16

    std::array<VkVertexInputBindingDescription, 2> bindings{ skinnedBind, instBind };
    std::vector<VkVertexInputAttributeDescription> allAttrs(skinnedAttrs.begin(), skinnedAttrs.end());
    allAttrs.insert(allAttrs.end(), instAttrs.begin(), instAttrs.end());

    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount   = 2;
    vi.pVertexBindingDescriptions      = bindings.data();
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(allAttrs.size());
    vi.pVertexAttributeDescriptions    = allAttrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rast{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rast.polygonMode = VK_POLYGON_MODE_FILL; rast.lineWidth = 1.0f;
    rast.cullMode = VK_CULL_MODE_NONE; rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // doubleSided:true

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState charDepthBlend{};
    charDepthBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState skinnedBlends[2] = {blendAttach, charDepthBlend};
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 2; cb.pAttachments = skinnedBlends;

    VkDescriptorSetLayout setLayouts[2] = { m_descriptors->getLayout(), m_bindless->getLayout() };
    VkPushConstantRange pc{};
    // Include FRAGMENT_BIT so this range is compatible with the water renderer's
    // VERTEX|FRAGMENT push constant range that overlaps at bytes [0..4].
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.size       = sizeof(uint32_t); // boneBaseIndex
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 2;
    lci.pSetLayouts            = setLayouts;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pc;
    VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &m_skinnedPipelineLayout), "skinned layout");

    VkPipelineRenderingCreateInfo skinnedDynCI = m_pipeline->getRenderFormats().pipelineRenderingCI();

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.pNext               = &skinnedDynCI;
    pci.stageCount          = 2;
    pci.pStages             = stages;
    pci.pVertexInputState   = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState      = &vps;
    pci.pRasterizationState = &rast;
    pci.pMultisampleState   = &ms;
    pci.pDepthStencilState  = &ds;
    pci.pColorBlendState    = &cb;
    pci.pDynamicState       = &dyn;
    pci.layout              = m_skinnedPipelineLayout;
    pci.renderPass          = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_skinnedPipeline), "skinned pipeline");

    vkDestroyShaderModule(dev, frag, nullptr);
    vkDestroyShaderModule(dev, vert, nullptr);
    spdlog::info("GPU-skinned pipeline created");
}

void Renderer::destroySkinnedPipeline() {
    VkDevice dev = m_device->getDevice();
    if (m_skinnedPipeline)       { vkDestroyPipeline(dev, m_skinnedPipeline, nullptr);            m_skinnedPipeline       = VK_NULL_HANDLE; }
    if (m_skinnedPipelineLayout) { vkDestroyPipelineLayout(dev, m_skinnedPipelineLayout, nullptr); m_skinnedPipelineLayout = VK_NULL_HANDLE; }
}

} // namespace glory
