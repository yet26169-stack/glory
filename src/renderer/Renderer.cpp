#include "renderer/Renderer.h"
#include "renderer/FogOfWarRenderer.h"
#include "renderer/VkCheck.h"
#include "renderer/Frustum.h"
#include "renderer/SceneBuilder.h"
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
#include <future>
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
    m_npcBehaviorSystem = std::make_unique<NPCBehaviorSystem>();
    m_respawnSystem = std::make_unique<RespawnSystem>();

    // Wire economy system into combat and projectile systems
    m_combatSystem->setEconomySystem(m_economySystem.get());
    m_projectileSystem->setEconomySystem(m_economySystem.get());

    // ── Audio engine ─────────────────────────────────────────────────────
    m_audioEngine.init();
    m_audioResources = std::make_unique<AudioResourceManager>(m_audioEngine);
    m_audioResources->setBasePath(std::string(ASSET_DIR) + "audio/");
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

    // ── Post-processing: SSAO (must be after HiZ init for pyramid view) ──
    {
        auto ext = m_window.getExtent();
        m_ssaoPass.init(*m_device, ext.width, ext.height,
                        m_hdrFB->depthView(), m_hdrFB->sampler());
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
    m_outlineRenderer.reset();
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

    m_currentDt = dt;

    // ── Simulation: preTick → ECS tick → postTick (FoW) ─────────────────
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
            .npcBehavior    = m_npcBehaviorSystem.get(),
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
            .debugRenderer      = &m_debugRenderer,
            .audioEngine        = &m_audioEngine,
            .audioResources     = m_audioResources.get(),
            .isoCam             = &m_isoCam,
            .vfxFactory         = &m_vfxFactory,
            .scriptEngine       = &m_scriptEngine,
            .dynamicObstacles   = &m_dynamicObstacles,
            .pathfinding        = &m_pathfinding,
            .fogOfWar           = m_fogOfWar.get(),
            .fogSystem          = &m_fogSystem,
            .fowGameplay        = &m_fowGameplay,
            .currentFrame       = m_currentFrame,
        };
        m_simLoop.step(simCtx);
        m_coneEffectTimer = simCtx.coneEffectTimer;
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

    // ── Flush VFX graveyards (safe after fence — GPU done with this slot) ──
    if (m_vfxRenderer)   m_vfxRenderer->flushGraveyard();
    if (m_trailRenderer) m_trailRenderer->flushGraveyard();

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
        m_boneSlotPool.assignAndUpload(m_scene.getRegistry(), *m_descriptors, m_currentFrame);
    }

    // ── Debug UI (ImGui) ─────────────────────────────────────────────────
    if (m_showDebugUI && !m_menuMode) {
        ImGui::Begin("Debug Tools", &m_showDebugUI);

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
    VK_CHECK(m_device->submitGraphics(1, &submitInfo),
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
    result = m_device->present(&pi);
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

    // ── Build per-frame context and execute the render graph ────────────────
    FrameContext ctx = buildFrameContext(cmd, imageIndex);
    m_renderGraph.execute(cmd, ctx);

    VK_CHECK(vkEndCommandBuffer(cmd), "End command buffer");
}

// ── recordShadowPass ─────────────────────────────────────────────────────────
void Renderer::buildSceneAsync() {
    m_buildSceneFuture = std::async(std::launch::async, [this]() {
        buildScene();
    });
}

bool Renderer::isBuildSceneDone() const {
    if (!m_buildSceneFuture.valid()) return true;  // never started or already consumed
    if (m_buildSceneFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        // Calling .get() re-throws any exception from the background thread.
        const_cast<std::future<void>&>(m_buildSceneFuture).get();
        return true;
    }
    return false;
}

// ── buildScene ───────────────────────────────────────────────────────────────
void Renderer::buildScene() {
    SceneBuilder::build(*this);
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

} // namespace glory
