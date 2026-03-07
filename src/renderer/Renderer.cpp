#include "renderer/Renderer.h"
#include "renderer/VkCheck.h"
#include "renderer/Model.h"
#include "renderer/StaticSkinnedMesh.h"
#include "renderer/Frustum.h"
#include "scene/Components.h"
#include "window/Window.h"

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

    m_descriptors = std::make_unique<Descriptors>(*m_device, Sync::MAX_FRAMES_IN_FLIGHT);
    // Single forward pass directly to swapchain (no HDR offscreen)
    m_pipeline = std::make_unique<Pipeline>(*m_device, *m_swapchain, m_descriptors->getLayout());
    m_sync = std::make_unique<Sync>(*m_device, m_swapchain->getImageCount());

    // Dummy 1×1 white texture bound to the shadow-map descriptor slot.
    // The shader's calcShadow() reads depth 1.0 from it → always returns 1.0 (no shadow).
    m_dummyShadow = Texture::createDefault(*m_device);
    m_descriptors->updateShadowMap(m_dummyShadow.getImageView(), m_dummyShadow.getSampler());

    m_clickIndicatorRenderer = std::make_unique<ClickIndicatorRenderer>(
        *m_device, m_pipeline->getRenderPass());
    m_debugRenderer.init(*m_device, m_pipeline->getRenderPass());

    createInstanceBuffers();
    createGridPipeline();
    createSkinnedPipeline();

    // ── VFX & Ability systems ─────────────────────────────────────────────
    m_vfxQueue      = std::make_unique<VFXEventQueue>();
    m_vfxRenderer   = std::make_unique<VFXRenderer>(*m_device, m_pipeline->getRenderPass());
    m_vfxRenderer->loadEmitterDirectory(std::string(ASSET_DIR) + "vfx/");
    m_abilitySystem = std::make_unique<AbilitySystem>(*m_vfxQueue);
    m_abilitySystem->loadDirectory(std::string(ASSET_DIR) + "abilities/");

    m_isoCam.setBounds(glm::vec3(0, 0, 0), glm::vec3(200, 0, 200));
    m_isoCam.setTarget(glm::vec3(100, 0, 100));

    buildScene();

    m_input = std::make_unique<InputManager>(m_window.getHandle(), m_camera);
    m_input->setCaptureEnabled(false); // MOBA mode: IsometricCamera drives view

    m_lastFrameTime = static_cast<float>(glfwGetTime());
    spdlog::info("Renderer initialized");
}

// ── Destructor ───────────────────────────────────────────────────────────────
Renderer::~Renderer() {
    if (m_device) vkDeviceWaitIdle(m_device->getDevice());

    m_input.reset();
    m_clickIndicatorRenderer.reset();
    m_vfxRenderer.reset();
    m_vfxQueue.reset();
    m_abilitySystem.reset();
    m_debugRenderer.cleanup();
    destroyInstanceBuffers();
    destroyGridPipeline();
    destroySkinnedPipeline();
    m_dummyShadow = Texture{};
    m_descriptors.reset();
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

// ── drawFrame ────────────────────────────────────────────────────────────────
void Renderer::drawFrame() {
    float currentTime = static_cast<float>(glfwGetTime());
    float dt = currentTime - m_lastFrameTime;
    m_lastFrameTime = currentTime;
    m_gameTime += dt;
    m_currentDt = dt;

    // ── VFX: flush event queue and update CPU emitters ────────────────────
    if (m_vfxRenderer && m_vfxQueue) {
        m_vfxRenderer->processQueue(*m_vfxQueue);
        m_vfxRenderer->update(dt);
    }
    if (m_abilitySystem) {
        m_abilitySystem->update(m_scene.getRegistry(), dt);
    }
    
    // ── Standard frame loop: Wait for GPU ─────────────────────────────────
    VkDevice dev = m_device->getDevice();
    VkFence fence = m_sync->getInFlightFence(m_currentFrame);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkSemaphore imgSem = m_sync->getImageAvailableSemaphore(m_currentFrame);
    VkResult result = vkAcquireNextImageKHR(dev, m_swapchain->getSwapchain(),
                                             UINT64_MAX, imgSem, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("Failed to acquire swapchain image");

    vkResetFences(dev, 1, &fence);
    
    m_debugRenderer.clear();

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

    // Right-click: move player to world position
    if (m_input->wasRightClicked() && m_playerEntity != entt::null) {
        glm::vec2 clickPos = m_input->getLastClickPos();
        glm::vec3 worldPos = screenToWorld(clickPos.x, clickPos.y);
        auto& c = m_scene.getRegistry().get<CharacterComponent>(m_playerEntity);
        c.targetPosition = worldPos;
        c.hasTarget = true;
        m_clickAnim = ClickAnim{ worldPos, 0.0f, 0.25f };
    }

    // ── Ability keys: Q / W / E / R ───────────────────────────────────────
    // Build a target from the current cursor world position (ground-targeted).
    // The AbilitySystem validates the targeting type; ground pos is always valid.
    if (m_abilitySystem && m_playerEntity != entt::null) {
        glm::vec2 mpos     = m_input->getMousePos();
        glm::vec3 worldPos = screenToWorld(mpos.x, mpos.y);
        TargetInfo groundTarget;
        groundTarget.type           = TargetingType::POINT;
        groundTarget.targetPosition = worldPos;

        auto tryAbility = [&](bool pressed, AbilitySlot slot) {
            if (!pressed) return;
            m_abilitySystem->enqueueRequest(m_playerEntity, slot, groundTarget);
        };
        tryAbility(m_input->wasQPressed(), AbilitySlot::Q);
        tryAbility(m_input->wasWPressed(), AbilitySlot::W);
        tryAbility(m_input->wasEPressed(), AbilitySlot::E);
        tryAbility(m_input->wasRPressed(), AbilitySlot::R);
    }

    // ── Unit System: Spawning (X) ──────────────────────────────────────────
    m_spawnTimer -= dt;
    if (glfwGetKey(m_window.getHandle(), GLFW_KEY_X) == GLFW_PRESS && m_spawnTimer <= 0.0f) {
        glm::vec2 mpos = m_input->getMousePos();
        glm::vec3 worldPos = screenToWorld(mpos.x, mpos.y);
        
        auto minion = m_scene.createEntity("MeleeMinion");
        auto& t = m_scene.getRegistry().get<TransformComponent>(minion);
        t.position = worldPos;
        t.scale    = glm::vec3(0.05f); // Match player scale
        t.rotation = glm::vec3(0.0f);

        m_scene.getRegistry().emplace<SelectableComponent>(minion, SelectableComponent{ false, 1.0f });
        m_scene.getRegistry().emplace<UnitComponent>(minion, UnitComponent{ UnitComponent::State::IDLE, worldPos, 5.0f });
        m_scene.getRegistry().emplace<CharacterComponent>(minion, CharacterComponent{ worldPos, 5.0f });
        m_scene.getRegistry().emplace<GPUSkinnedMeshComponent>(minion, GPUSkinnedMeshComponent{ m_minionMeshIndex });

        // Setup simple Material
        uint32_t flatNorm = 0; // assuming 0 is flatNorm from buildScene
        m_scene.getRegistry().emplace<MaterialComponent>(minion,
            MaterialComponent{ m_minionTexIndex, flatNorm, 0.0f, 0.0f, 0.5f, 0.2f });
        // Setup Animation (Melee minion)
        SkeletonComponent skelComp;
        skelComp.skeleton         = m_minionSkeleton;
        skelComp.skinVertices     = m_minionSkinVertices;
        skelComp.bindPoseVertices = m_minionBindPoseVertices;

        AnimationComponent animComp;
        animComp.player.setSkeleton(&skelComp.skeleton);
        animComp.clips = m_minionClips;
        if (!animComp.clips.empty()) {
            animComp.activeClipIndex = 0;
            animComp.player.setClip(&animComp.clips[0]);
        }

        m_scene.getRegistry().emplace<SkeletonComponent>(minion, std::move(skelComp));
        m_scene.getRegistry().emplace<AnimationComponent>(minion, std::move(animComp));

        m_spawnTimer = 0.5f; // Debounce
        spdlog::info("Spawned minion at ({}, {}, {})", worldPos.x, worldPos.y, worldPos.z);
    }

    // ── Unit System: Selection (Marquee) ───────────────────────────────────
    if (m_input->isLeftMouseDown()) {
        if (!m_selection.isDragging) {
            m_selection.isDragging = true;
            m_selection.dragStart = m_input->getMousePos();
        }
        m_selection.dragEnd = m_input->getMousePos();
        
        // Draw Marquee Box using DebugRenderer on the ground plane
        glm::vec3 tl = screenToWorld(m_selection.dragStart.x, m_selection.dragStart.y);
        glm::vec3 tr = screenToWorld(m_selection.dragEnd.x, m_selection.dragStart.y);
        glm::vec3 br = screenToWorld(m_selection.dragEnd.x, m_selection.dragEnd.y);
        glm::vec3 bl = screenToWorld(m_selection.dragStart.x, m_selection.dragEnd.y);
        
        // Offset slightly above ground to prevent z-fighting
        tl.y = tr.y = br.y = bl.y = 0.05f;
        
        glm::vec4 color(0.2f, 1.0f, 0.4f, 1.0f);
        m_debugRenderer.drawLine(tl, tr, color);
        m_debugRenderer.drawLine(tr, br, color);
        m_debugRenderer.drawLine(br, bl, color);
        m_debugRenderer.drawLine(bl, tl, color);
        
    } else {
        if (m_selection.isDragging) {
            m_selection.isDragging = false;
            glm::vec2 start = m_selection.dragStart;
            glm::vec2 end   = m_selection.dragEnd;
            
            float dist = glm::distance(start, end);
            bool isClick = dist < 5.0f;

            auto view = m_scene.getRegistry().view<TransformComponent, SelectableComponent>();
            glm::vec2 min = glm::min(start, end);
            glm::vec2 max = glm::max(start, end);

            if (isClick) {
                // Determine if we clicked on a unit
                bool clickedOnUnit = false;
                for (auto e : view) {
                    auto& t = view.get<TransformComponent>(e);
                    if (glm::distance(worldToScreen(t.position), end) < 20.0f) {
                        clickedOnUnit = true;
                        break;
                    }
                }

                if (clickedOnUnit) {
                    // Select clicked unit(s)
                    for (auto e : view) {
                        auto& t = view.get<TransformComponent>(e);
                        auto& s = view.get<SelectableComponent>(e);
                        if (glm::distance(worldToScreen(t.position), end) < 20.0f) {
                            s.isSelected = true;
                        } else {
                            if (!glfwGetKey(m_window.getHandle(), GLFW_KEY_LEFT_SHIFT)) s.isSelected = false;
                        }
                    }
                } else {
                    // Clicked on ground -> move selected units
                    glm::vec3 targetWorld = screenToWorld(end.x, end.y);
                    auto unitView = m_scene.getRegistry().view<SelectableComponent, CharacterComponent, UnitComponent>();
                    
                    int numSelected = 0;
                    for (auto e : unitView) {
                        if (unitView.get<SelectableComponent>(e).isSelected) numSelected++;
                    }

                    int unitIndex = 0;
                    for (auto e : unitView) {
                        auto& s = unitView.get<SelectableComponent>(e);
                        if (s.isSelected) {
                            auto& c = unitView.get<CharacterComponent>(e);
                            auto& u = unitView.get<UnitComponent>(e);
                            
                            // Calculate simple circular formation offset
                            glm::vec3 offset(0.0f);
                            if (numSelected > 1) {
                                float radius = 1.0f + (numSelected * 0.2f);
                                float angle = (6.2831853f / numSelected) * unitIndex;
                                offset = glm::vec3(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);
                            }

                            c.targetPosition = targetWorld + offset;
                            c.hasTarget = true;
                            u.state = UnitComponent::State::MOVING;
                            u.targetPosition = targetWorld + offset;
                            unitIndex++;
                        }
                    }
                }
            } else {
                // Marquee selection
                for (auto e : view) {
                    auto& t = view.get<TransformComponent>(e);
                    auto& s = view.get<SelectableComponent>(e);
                    glm::vec2 screenPos = worldToScreen(t.position);
                    if (screenPos.x >= min.x && screenPos.x <= max.x &&
                        screenPos.y >= min.y && screenPos.y <= max.y) {
                        s.isSelected = true;
                    } else {
                        if (!glfwGetKey(m_window.getHandle(), GLFW_KEY_LEFT_SHIFT)) s.isSelected = false;
                    }
                }
            }
        }
    }

    // ── Update minions movement ───────────────────────────────────────────
    auto minionView = m_scene.getRegistry().view<UnitComponent, CharacterComponent, TransformComponent>();
    for (auto e : minionView) {
        if (e == m_playerEntity) continue;
        auto& c = minionView.get<CharacterComponent>(e);
        auto& t = minionView.get<TransformComponent>(e);
        auto& u = minionView.get<UnitComponent>(e);

        const float accelRate = 30.0f;

        if (c.hasTarget) {
            glm::vec3 dir = c.targetPosition - t.position;
            dir.y = 0.0f;
            float dist = glm::length(dir);
            if (dist < 0.05f) {
                t.position.x = c.targetPosition.x;
                t.position.z = c.targetPosition.z;
                c.hasTarget = false;
                u.state = UnitComponent::State::IDLE;
            } else {
                dir /= dist;
                // League-style: snap rotation instantly to target direction
                c.currentYaw = std::atan2(dir.x, dir.z);
                t.rotation.y = c.currentYaw;

                // Fast acceleration toward full speed
                c.currentSpeed += (c.moveSpeed - c.currentSpeed) * std::min(accelRate * dt, 1.0f);

                float step = c.currentSpeed * dt;
                if (step >= dist) {
                    t.position.x = c.targetPosition.x;
                    t.position.z = c.targetPosition.z;
                    c.hasTarget = false;
                    u.state = UnitComponent::State::IDLE;
                } else {
                    t.position += dir * step;
                    u.state = UnitComponent::State::MOVING;
                }
            }
        } else {
            c.currentSpeed *= std::max(1.0f - 30.0f * dt, 0.0f);
        }
    }

    // ── Update character movement ─────────────────────────────────────────
    if (m_playerEntity != entt::null &&
        m_scene.getRegistry().valid(m_playerEntity) &&
        m_scene.getRegistry().all_of<CharacterComponent, TransformComponent>(m_playerEntity)) {

        const float accelRate = 30.0f;

        auto& c = m_scene.getRegistry().get<CharacterComponent>(m_playerEntity);
        auto& t = m_scene.getRegistry().get<TransformComponent>(m_playerEntity);
        if (c.hasTarget) {
            glm::vec3 dir = c.targetPosition - t.position;
            dir.y = 0.0f;
            float dist = glm::length(dir);
            if (dist < 0.05f) {
                t.position.x = c.targetPosition.x;
                t.position.z = c.targetPosition.z;
                c.hasTarget = false;
            } else {
                dir /= dist;
                // League-style: snap rotation instantly to target direction
                c.currentYaw = std::atan2(dir.x, dir.z);
                t.rotation.y = c.currentYaw;

                // Fast acceleration toward full speed
                c.currentSpeed += (c.moveSpeed - c.currentSpeed) * std::min(accelRate * dt, 1.0f);

                // Clamp step to remaining distance to prevent overshoot
                float step = c.currentSpeed * dt;
                if (step >= dist) {
                    t.position.x = c.targetPosition.x;
                    t.position.z = c.targetPosition.z;
                    c.hasTarget = false;
                } else {
                    t.position += dir * step;
                }
            }
        } else {
            // Decelerate to stop
            c.currentSpeed *= std::max(1.0f - 30.0f * dt, 0.0f);
        }
        m_isoCam.setFollowTarget(t.position);
    }

    // ── Update animations ─────────────────────────────────────────────────
    auto animView = m_scene.getRegistry()
        .view<SkeletonComponent, AnimationComponent, GPUSkinnedMeshComponent, TransformComponent>();
    uint32_t currentBoneSlot = 0;
    for (auto&& [e, skel, anim, ssm, t] : animView.each()) {
        // Switch clip based on movement state (0=idle, 1=walk) with crossfade
        if (m_scene.getRegistry().all_of<CharacterComponent>(e)) {
            auto& c = m_scene.getRegistry().get<CharacterComponent>(e);
            int targetClip = c.hasTarget ? 1 : 0;
            if (anim.activeClipIndex != targetClip && targetClip < (int)anim.clips.size()) {
                anim.activeClipIndex = targetClip;
                anim.player.crossfadeTo(&anim.clips[targetClip], 0.15f);
            }
            // Scale walk animation speed to match movement (eliminates foot-slide).
            // If the clip has a strideLength calibrated, use the biomechanically
            // correct ratio: R = V_phys / (L_s / T_cycle).  Otherwise fall back to
            // the simpler currentSpeed/moveSpeed ratio which works when moveSpeed
            // already matches the animation's natural pace.
            if (anim.activeClipIndex == 1 && !anim.player.isBlending() &&
                anim.activeClipIndex < (int)anim.clips.size()) {
                const auto& walkClip = anim.clips[anim.activeClipIndex];
                float timeScale = 1.0f;
                if (walkClip.strideLength > 0.0f && walkClip.duration > 0.0f) {
                    float animNaturalSpeed = walkClip.strideLength / walkClip.duration;
                    timeScale = (animNaturalSpeed > 0.0f) ? (c.currentSpeed / animNaturalSpeed) : 1.0f;
                } else if (c.moveSpeed > 0.0f) {
                    timeScale = c.currentSpeed / c.moveSpeed;
                }
                anim.player.setTimeScale(timeScale);
            } else {
                anim.player.setTimeScale(1.0f);
            }
        }
        anim.player.refreshSkeleton(&skel.skeleton);
        anim.player.update(dt);
        const auto& matrices = anim.player.getSkinningMatrices();
        m_descriptors->writeBoneSlot(m_currentFrame, currentBoneSlot, matrices);
        ssm.boneSlot = currentBoneSlot++;
    }

    // ── Click animation ───────────────────────────────────────────────────
    if (m_clickAnim) {
        m_clickAnim->lifetime += dt;
        if (m_clickAnim->lifetime >= m_clickAnim->maxLife)
            m_clickAnim.reset();
    }

    VkCommandBuffer cmd = m_sync->getCommandBuffer(m_currentFrame);
    vkResetCommandBuffer(cmd, 0);
    recordCommandBuffer(cmd, imageIndex, dt);

    VkSemaphore     waitSems[]   = { imgSem };
    VkPipelineStageFlags stages[]= { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore     sigSems[]    = { m_sync->getRenderFinishedSemaphore(imageIndex) };

    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = waitSems;
    si.pWaitDstStageMask    = stages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = sigSems;
    VK_CHECK(vkQueueSubmit(m_device->getGraphicsQueue(), 1, &si, fence), "Queue submit failed");

    VkSwapchainKHR swapchains[] = { m_swapchain->getSwapchain() };
    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = sigSems;
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

// ── recordCommandBuffer ──────────────────────────────────────────────────────
void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, float /*dt*/) {
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi), "Begin command buffer");

    // ── VFX compute pass (particle simulation, OUTSIDE render pass) ──────
    if (m_vfxRenderer) {
        m_vfxRenderer->dispatchCompute(cmd);
        m_vfxRenderer->barrierComputeToGraphics(cmd);
    }

    auto ext = m_swapchain->getExtent();
    float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);

    // ── Update per-frame UBO ─────────────────────────────────────────────
    {
        UniformBufferObject ubo{};
        ubo.view             = m_isoCam.getViewMatrix();
        ubo.proj             = m_isoCam.getProjectionMatrix(aspect);
        ubo.lightSpaceMatrix = glm::mat4(1.0f); // no real shadows
        m_descriptors->updateUniformBuffer(m_currentFrame, ubo);
    }
    {
        LightUBO light{};
        light.viewPos = m_isoCam.getPosition();
        light.lightCount = 1;
        light.ambientStrength = 0.5f;
        light.lights[0].position = glm::vec3(100.0f, 60.0f, 100.0f);
        light.lights[0].color    = glm::vec3(1.0f, 0.95f, 0.85f);
        m_descriptors->updateLightBuffer(m_currentFrame, light);
    }

    // ── Begin render pass ────────────────────────────────────────────────
    std::array<VkClearValue, 2> clears{};
    clears[0].color        = {{ 0.08f, 0.10f, 0.14f, 1.0f }};
    clears[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass       = m_pipeline->getRenderPass();
    rp.framebuffer      = m_pipeline->getFramebuffer(imageIndex);
    rp.renderArea       = { {0, 0}, ext };
    rp.clearValueCount  = static_cast<uint32_t>(clears.size());
    rp.pClearValues     = clears.data();
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
    VkRect2D   sc{ {0, 0}, ext };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // ── Static mesh pass ─────────────────────────────────────────────────
    VkPipeline mainPipeline = m_wireframe ? m_pipeline->getWireframePipeline()
                                          : m_pipeline->getGraphicsPipeline();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mainPipeline);
    VkDescriptorSet ds = m_descriptors->getSet(m_currentFrame);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipeline->getPipelineLayout(), 0, 1, &ds, 0, nullptr);

    auto* instances = static_cast<InstanceData*>(m_instanceMapped[m_currentFrame]);
    uint32_t instanceIndex = 0;

    auto staticView = m_scene.getRegistry()
        .view<TransformComponent, MeshComponent, MaterialComponent>(
            entt::exclude<GPUSkinnedMeshComponent>);
    for (auto&& [e, t, mc, mat] : staticView.each()) {
        if (instanceIndex >= MAX_INSTANCES) break;
        glm::mat4 model = t.getModelMatrix();
        instances[instanceIndex].model        = model;
        instances[instanceIndex].normalMatrix = glm::transpose(glm::inverse(model));
        instances[instanceIndex].tint         = glm::vec4(1.0f);
        instances[instanceIndex].params       = glm::vec4(mat.shininess, mat.metallic, mat.roughness, mat.emissive);
        instances[instanceIndex].texIndices   = glm::vec4(
            static_cast<float>(mat.materialIndex), static_cast<float>(mat.normalMapIndex), 0.0f, 0.0f);

        VkBuffer     instBuf    = m_instanceBuffers[m_currentFrame].getBuffer();
        VkDeviceSize instOffset = instanceIndex * sizeof(InstanceData);
        vkCmdBindVertexBuffers(cmd, 1, 1, &instBuf, &instOffset);
        m_scene.getMesh(mc.meshIndex).draw(cmd);
        ++instanceIndex;
    }

    // ── GPU-skinned pass ─────────────────────────────────────────────────
    if (m_skinnedPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_skinnedPipelineLayout, 0, 1, &ds, 0, nullptr);

        auto skinnedView = m_scene.getRegistry()
            .view<TransformComponent, MaterialComponent, GPUSkinnedMeshComponent>();
        for (auto&& [e, t, mat, ssm] : skinnedView.each()) {
            if (instanceIndex >= MAX_INSTANCES) break;
            glm::mat4 model = t.getModelMatrix();
            instances[instanceIndex].model        = model;
            instances[instanceIndex].normalMatrix = glm::transpose(glm::inverse(model));
            
            // Highlight selected units with a green tint
            glm::vec4 tint(1.0f);
            if (m_scene.getRegistry().all_of<SelectableComponent>(e)) {
                if (m_scene.getRegistry().get<SelectableComponent>(e).isSelected) {
                    tint = glm::vec4(0.5f, 1.0f, 0.5f, 1.0f);
                }
            }
            instances[instanceIndex].tint = tint;

            instances[instanceIndex].params = glm::vec4(mat.shininess, mat.metallic, mat.roughness, mat.emissive);
            instances[instanceIndex].texIndices   = glm::vec4(
                static_cast<float>(mat.materialIndex), static_cast<float>(mat.normalMapIndex), 0.0f, 0.0f);

            VkBuffer     instBuf    = m_instanceBuffers[m_currentFrame].getBuffer();
            VkDeviceSize instOffset = instanceIndex * sizeof(InstanceData);
            vkCmdBindVertexBuffers(cmd, 1, 1, &instBuf, &instOffset);

            uint32_t boneBase = ssm.boneSlot * Descriptors::MAX_BONES;
            vkCmdPushConstants(cmd, m_skinnedPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t), &boneBase);

            const auto& mesh = m_scene.getStaticSkinnedMesh(ssm.staticSkinnedMeshIndex);
            mesh.bind(cmd);
            mesh.draw(cmd);
            ++instanceIndex;
        }
    }

    // ── Debug grid ───────────────────────────────────────────────────────
    if (m_showGrid && m_gridPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_gridPipeline);
        struct GridPC { glm::mat4 viewProj; float gridY; } gridPC;
        gridPC.viewProj = m_isoCam.getProjectionMatrix(aspect) * m_isoCam.getViewMatrix();
        gridPC.gridY    = 0.0f;
        vkCmdPushConstants(cmd, m_gridPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(GridPC), &gridPC);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    // ── Click indicator ───────────────────────────────────────────────────
    if (m_clickAnim && m_clickIndicatorRenderer) {
        float t_norm = m_clickAnim->lifetime / m_clickAnim->maxLife;
        glm::mat4 vp = m_isoCam.getProjectionMatrix(aspect) * m_isoCam.getViewMatrix();
        m_clickIndicatorRenderer->render(cmd, vp, m_clickAnim->position, t_norm, 1.5f);
    }

    // ── Debug Renderer (Marquee Box) ──────────────────────────────────────
    glm::mat4 viewProj = m_isoCam.getProjectionMatrix(aspect) * m_isoCam.getViewMatrix();
    m_debugRenderer.render(cmd, viewProj);

    // ── VFX particle render pass ──────────────────────────────────────────
    if (m_vfxRenderer) {
        const glm::mat4& view = m_isoCam.getViewMatrix();
        // Extract camera right and up from the view matrix rows
        glm::vec3 camRight{view[0][0], view[1][0], view[2][0]};
        glm::vec3 camUp   {view[0][1], view[1][1], view[2][1]};
        m_vfxRenderer->render(cmd, viewProj, camRight, camUp);
    }

    vkCmdEndRenderPass(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd), "End command buffer");
}

// ── buildScene ───────────────────────────────────────────────────────────────
void Renderer::buildScene() {
    // Default textures
    uint32_t defaultTex  = m_scene.addTexture(Texture::createDefault(*m_device));
    uint32_t checkerTex  = m_scene.addTexture(Texture::createCheckerboard(*m_device));
    uint32_t flatNorm    = m_scene.addTexture(Texture::createFlatNormal(*m_device));

    // Bind to bindless descriptor array
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_scene.getTextures().size()); ++i) {
        auto& tex = m_scene.getTexture(i);
        m_descriptors->writeBindlessTexture(i, tex.getImageView(), tex.getSampler());
    }

    // ── Flat map (200×200 units, flat terrain) ────────────────────────────
    uint32_t mapMesh = m_scene.addMesh(
        Model::createTerrain(*m_device, m_device->getAllocator(), 200.0f, 64, 0.0f));
    auto map = m_scene.createEntity("FlatMap");
    m_scene.getRegistry().emplace<MeshComponent>(map, MeshComponent{ mapMesh });
    m_scene.getRegistry().emplace<MaterialComponent>(map,
        MaterialComponent{ checkerTex, flatNorm, 0.0f, 0.0f, 0.9f, 0.0f });
    auto& mapT = m_scene.getRegistry().get<TransformComponent>(map);
    mapT.position = glm::vec3(100.0f, 0.0f, 100.0f);

    // ── Player character ─────────────────────────────────────────────────
    auto character = m_scene.createEntity("PlayerCharacter");
    bool skinnedLoaded = false;

    try {
        std::string charPath = std::string(MODEL_DIR) + "models/scientist/scientist.glb";
        auto skinnedData = Model::loadSkinnedFromGLB(
            *m_device, m_device->getAllocator(), charPath, 0.0f);

        // Load textures
        uint32_t charTex = defaultTex;
        auto glbTextures = Model::loadGLBTextures(*m_device, charPath);
        if (!glbTextures.empty()) {
            charTex = m_scene.addTexture(std::move(glbTextures[0].texture));
            m_descriptors->writeBindlessTexture(charTex, 
                m_scene.getTexture(charTex).getImageView(),
                m_scene.getTexture(charTex).getSampler());
            spdlog::info("Character texture loaded at slot {}", charTex);
        } else {
            spdlog::warn("Character texture not found in GLB — using default white texture (slot {})", charTex);
        }

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
                } catch (...) {}
                return false;
            };

            std::string base = std::string(MODEL_DIR) + "models/scientist/";
            bool idleOk = tryLoadAnim(base + "scientist_idle.glb");
            if (!idleOk && !skinnedData.animations.empty())
                animComp.clips.push_back(skinnedData.animations[0]); // embedded fallback
            tryLoadAnim(base + "scientist_walk.glb");

            if (!animComp.clips.empty()) {
                animComp.activeClipIndex = 0;
                animComp.player.setClip(&animComp.clips[0]);
            }

            m_scene.getRegistry().emplace<SkeletonComponent>(character, std::move(skelComp));
            m_scene.getRegistry().emplace<AnimationComponent>(character, std::move(animComp));
            m_scene.getRegistry().emplace<GPUSkinnedMeshComponent>(character,
                GPUSkinnedMeshComponent{ ssIdx });
            m_scene.getRegistry().emplace<MaterialComponent>(character,
                MaterialComponent{ charTex, flatNorm, 0.0f, 0.0f, 0.5f, 0.2f });

            auto& ct = m_scene.getRegistry().get<TransformComponent>(character);
            ct.position  = glm::vec3(100.0f, 0.0f, 100.0f);
            ct.scale     = glm::vec3(0.05f); // Increased scale
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

    m_scene.getRegistry().emplace<CharacterComponent>(character,
        CharacterComponent{ glm::vec3(100.0f, 0.0f, 100.0f), 8.0f });
    m_playerEntity = character;
    m_isoCam.setFollowTarget(glm::vec3(100.0f, 0.0f, 100.0f));

    // ── Load Minion assets for spawning ───────────────────────────────────
    try {
        std::string minionPath = std::string(MODEL_DIR) + "models/melee_minion/melee_minion_walking.glb";
        auto skinnedData = Model::loadSkinnedFromGLB(*m_device, m_device->getAllocator(), minionPath, 0.0f);

        uint32_t minionTex = defaultTex;
        auto glbTextures = Model::loadGLBTextures(*m_device, minionPath);
        if (!glbTextures.empty()) {
            minionTex = m_scene.addTexture(std::move(glbTextures[0].texture));
            m_descriptors->writeBindlessTexture(minionTex,
                m_scene.getTexture(minionTex).getImageView(),
                m_scene.getTexture(minionTex).getSampler());
            spdlog::info("Minion texture loaded at slot {}", minionTex);
        } else {
            spdlog::warn("Minion texture not found in GLB — using default white texture (slot {})", minionTex);
        }
        m_minionTexIndex = minionTex;

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
            m_minionMeshIndex = m_scene.addStaticSkinnedMesh(
                StaticSkinnedMesh(*m_device, m_device->getAllocator(),
                                  sverts, skinnedData.indices[0]));

            m_minionSkeleton         = std::move(skinnedData.skeleton);
            m_minionSkinVertices     = std::move(skinnedData.skinVertices);
            m_minionBindPoseVertices = std::move(skinnedData.bindPoseVertices);

            // Load idle animation from melee_minion_idle.glb (clip[0])
            // Falls back to empty clip if file not found
            std::string minionAnimBase = std::string(MODEL_DIR) + "models/melee_minion/";
            bool idleOk = false;
            try {
                auto idleData = Model::loadSkinnedFromGLB(
                    *m_device, m_device->getAllocator(),
                    minionAnimBase + "melee_minion_idle.glb", 0.0f);
                if (!idleData.animations.empty()) {
                    m_minionClips.push_back(std::move(idleData.animations[0]));
                    retargetClip(m_minionClips.back(), m_minionSkeleton);
                    idleOk = true;
                    spdlog::info("Minion idle animation loaded and retargeted");
                }
            } catch (const std::exception& ie) {
                spdlog::warn("Could not load minion idle animation: {}", ie.what());
            }
            if (!idleOk)
                m_minionClips.push_back(AnimationClip{}); // empty fallback

            // Walk animation is embedded in the base model (clip[1])
            if (!skinnedData.animations.empty()) {
                m_minionClips.push_back(std::move(skinnedData.animations[0]));
            } else {
                spdlog::warn("No walk animation found in minion base model.");
            }

            spdlog::info("Minion model and {} animations loaded for spawning", m_minionClips.size());
        }    } catch (const std::exception& e) {
        spdlog::warn("Could not load minion model: {}", e.what());
    }

    spdlog::info("Scene built: flat map + player character");
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
    m_pipeline->recreateFramebuffers(*m_swapchain);
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
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE; ds.depthCompareOp = VK_COMPARE_OP_LESS;

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
    cb.attachmentCount = 1; cb.pAttachments = &blend;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.size       = sizeof(glm::mat4) + sizeof(float);
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pc;
    VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &m_gridPipelineLayout), "grid layout");

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
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
    pci.renderPass          = m_pipeline->getRenderPass();
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
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &blendAttach;

    VkDescriptorSetLayout descLayout = m_descriptors->getLayout();
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.size       = sizeof(uint32_t); // boneBaseIndex
    VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount         = 1;
    lci.pSetLayouts            = &descLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges    = &pc;
    VK_CHECK(vkCreatePipelineLayout(dev, &lci, nullptr, &m_skinnedPipelineLayout), "skinned layout");

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
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
    pci.renderPass          = m_pipeline->getRenderPass();
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
