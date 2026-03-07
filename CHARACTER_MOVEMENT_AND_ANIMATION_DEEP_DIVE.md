# Glory Engine: Character Movement & Animation Deep Dive

**Comprehensive Technical Documentation of Point-and-Click Movement System**  
*Complete code walkthrough covering input, raycasting, movement, rotation, animation state machine, skeletal animation, GPU skinning, and camera follow logic*

---

## Table of Contents

1. [Input System](#input-system)
2. [Component Architecture](#component-architecture)
3. [Main Renderer Loop & Movement Update](#main-renderer-loop)
4. [Rotation & Smooth Facing](#rotation-smooth-facing)
5. [Animation State Machine & Crossfading](#animation-state-machine)
6. [Skeletal Animation Pipeline](#skeletal-animation-pipeline)
7. [GPU Skinning](#gpu-skinning)
8. [Camera Follow System](#camera-follow-system)
9. [Visual Feedback (Click Indicator)](#visual-feedback)
10. [Selection & Formation Movement](#selection-formation)
11. [Minion Spawning & Unit System](#minion-spawning)

---

## Input System

### InputManager.h – Event Capture & Mouse Tracking

```cpp
#pragma once

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <utility>

namespace glory {

class Camera;

class InputManager {
public:
  InputManager(GLFWwindow *window, Camera &camera);
  ~InputManager() = default;

  void update(float deltaTime);

  bool isCursorCaptured() const { return m_cursorCaptured; }
  void setCaptureEnabled(bool enabled) { m_captureEnabled = enabled; }

  // Query methods – return true once per event, then reset to false
  bool wasF4Pressed();      // Grid toggle
  bool wasYPressed();       // (unused)
  bool wasRightClicked();   // Movement command
  glm::vec2 getLastClickPos() const { return m_rightClickPos; }
  bool wasLeftClicked();    // Selection
  glm::vec2 getLastLeftClickPos() const { return m_leftClickPos; }
  
  // Real-time state queries
  bool isLeftMouseDown() const { return glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS; }
  bool isRightMouseDown() const { return glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS; }
  
  // Current mouse position in screen space (pixels)
  glm::vec2 getMousePos() const {
    double x, y;
    glfwGetCursorPos(m_window, &x, &y);
    return glm::vec2(static_cast<float>(x), static_cast<float>(y));
  }

  // Scroll wheel accumulation for camera zoom
  float consumeScrollDelta() {
    float delta = m_scrollDelta;
    m_scrollDelta = 0.0f;
    return delta;
  }

private:
  GLFWwindow *m_window;
  Camera &m_camera;

  double m_lastX = 0.0;
  double m_lastY = 0.0;
  bool m_firstMouse = true;
  bool m_cursorCaptured = false;
  bool m_captureEnabled = true;  // Disabled for MOBA mode (IsometricCamera drives view)
  
  // One-shot flags reset after query
  bool m_f4Pressed = false;
  bool m_yPressed = false;
  bool m_rightClicked = false;
  glm::vec2 m_rightClickPos{0.0f};
  bool m_leftClicked = false;
  glm::vec2 m_leftClickPos{0.0f};

  float m_scrollDelta = 0.0f;

  // Static callbacks registered with GLFW
  static void keyCallback(GLFWwindow *window, int key, int scancode, int action, int mods);
  static void mouseCallback(GLFWwindow *window, double xPos, double yPos);
  static void scrollCallback(GLFWwindow *window, double xOffset, double yOffset);
  static void mouseButtonCallback(GLFWwindow *window, int button, int action, int mods);
};

} // namespace glory
```

**Key Points:**
- **M-MOBA Mode**: `setCaptureEnabled(false)` disables cursor capture; right-click triggers movement command instead of FPS-style camera look
- **One-shot events**: `wasRightClicked()` returns true once, then automatically resets; prevents repeated triggers from single click
- **Scroll accumulation**: `m_scrollDelta` tracks scroll wheel delta for camera zoom (consumed by IsometricCamera each frame)

### InputManager.cpp – GLFW Callbacks Implementation

```cpp
#include "input/InputManager.h"
#include "camera/Camera.h"

namespace glory {

static InputManager *s_activeInput = nullptr;

InputManager::InputManager(GLFWwindow *window, Camera &camera)
    : m_window(window), m_camera(camera) {
  s_activeInput = this;
  glfwSetKeyCallback(m_window, keyCallback);
  glfwSetCursorPosCallback(m_window, mouseCallback);
  glfwSetScrollCallback(m_window, scrollCallback);
  glfwSetMouseButtonCallback(m_window, mouseButtonCallback);
}

// One-shot flag getters – reset after returning
bool InputManager::wasF4Pressed() { 
  bool p = m_f4Pressed; 
  m_f4Pressed = false; 
  return p; 
}

bool InputManager::wasYPressed()  { 
  bool p = m_yPressed;  
  m_yPressed  = false; 
  return p; 
}

bool InputManager::wasRightClicked() { 
  bool p = m_rightClicked; 
  m_rightClicked = false; 
  return p; 
}

bool InputManager::wasLeftClicked()  { 
  bool p = m_leftClicked;  
  m_leftClicked  = false; 
  return p; 
}

void InputManager::keyCallback(GLFWwindow*, int key, int, int action, int) {
  if (!s_activeInput) return;
  
  // F4: Toggle debug grid
  if (key == GLFW_KEY_F4 && action == GLFW_PRESS) 
    s_activeInput->m_f4Pressed = true;
  
  // Y: (unused in current MOBA mode)
  if (key == GLFW_KEY_Y  && action == GLFW_PRESS) 
    s_activeInput->m_yPressed  = true;
  
  // ESC: Release cursor capture if in FPS mode
  if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && s_activeInput->m_cursorCaptured) {
    s_activeInput->m_cursorCaptured = false;
    glfwSetInputMode(s_activeInput->m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    s_activeInput->m_firstMouse = true;
  }
}

void InputManager::update(float deltaTime) {
  // Only used when cursor capture is enabled (FPS mode)
  if (m_captureEnabled && m_cursorCaptured) {
    bool fw  = glfwGetKey(m_window, GLFW_KEY_W)          == GLFW_PRESS;
    bool bw  = glfwGetKey(m_window, GLFW_KEY_S)          == GLFW_PRESS;
    bool lt  = glfwGetKey(m_window, GLFW_KEY_A)          == GLFW_PRESS;
    bool rt  = glfwGetKey(m_window, GLFW_KEY_D)          == GLFW_PRESS;
    bool up  = glfwGetKey(m_window, GLFW_KEY_SPACE)      == GLFW_PRESS;
    bool dn  = glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
    m_camera.processKeyboard(deltaTime, fw, bw, lt, rt, up, dn);
  }
}

// Mouse movement callback (unused in MOBA mode since capture is disabled)
void InputManager::mouseCallback(GLFWwindow*, double xPos, double yPos) {
  if (!s_activeInput || !s_activeInput->m_cursorCaptured) return;
  auto* self = s_activeInput;
  if (self->m_firstMouse) { 
    self->m_lastX = xPos; 
    self->m_lastY = yPos; 
    self->m_firstMouse = false; 
  }
  float dx = static_cast<float>(xPos - self->m_lastX);
  float dy = static_cast<float>(self->m_lastY - yPos);
  self->m_lastX = xPos; 
  self->m_lastY = yPos;
  self->m_camera.processMouse(dx, dy);
}

// Scroll callback – accumulates delta for camera zoom
void InputManager::scrollCallback(GLFWwindow*, double, double yOffset) {
  if (!s_activeInput) return;
  float d = static_cast<float>(yOffset);
  s_activeInput->m_scrollDelta += d;
  if (s_activeInput->m_cursorCaptured) 
    s_activeInput->m_camera.processScroll(d);
}

// Mouse button callback – triggers movement/selection in MOBA mode
void InputManager::mouseButtonCallback(GLFWwindow* window, int button, int action, int) {
  if (!s_activeInput) return;
  
  // RIGHT CLICK: Movement command in MOBA mode
  if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
    if (s_activeInput->m_captureEnabled) {
      // FPS mode: capture cursor
      s_activeInput->m_cursorCaptured = true;
      glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      s_activeInput->m_firstMouse = true;
    } else {
      // MOBA mode: record right-click position for movement raycasting
      double mx, my; 
      glfwGetCursorPos(window, &mx, &my);
      s_activeInput->m_rightClickPos = glm::vec2(static_cast<float>(mx), static_cast<float>(my));
      s_activeInput->m_rightClicked = true;
    }
  }
  
  // LEFT CLICK: Selection/marquee in MOBA mode
  if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS && !s_activeInput->m_captureEnabled) {
    double mx, my; 
    glfwGetCursorPos(window, &mx, &my);
    s_activeInput->m_leftClickPos = glm::vec2(static_cast<float>(mx), static_cast<float>(my));
    s_activeInput->m_leftClicked = true;
  }
}

} // namespace glory
```

**Callback Flow:**
1. Right-click in MOBA mode → `mouseButtonCallback` sets `m_rightClicked = true` and records screen position
2. Main loop queries `wasRightClicked()` → returns true, resets flag, queries position
3. Position is unproject to world space via `screenToWorld()` → sets character's `targetPosition`

---

## Component Architecture

### Components.h – Data-Driven Entity Structure

The Glory engine uses EnTT (entity component system) to organize character state. Each character entity carries multiple lightweight components:

```cpp
#pragma once

#include "animation/AnimationClip.h"
#include "animation/AnimationPlayer.h"
#include "animation/Skeleton.h"
#include "renderer/Buffer.h"
#include "renderer/Frustum.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>

namespace glory {

// ── Transform ────────────────────────────────────────────────────────────────
struct TransformComponent {
  glm::vec3 position{0.0f};        // World-space XYZ
  glm::vec3 rotation{0.0f};        // Euler angles in radians (Y-X-Z order)
  glm::vec3 scale{1.0f};           // Per-axis scale

  glm::mat4 getModelMatrix() const {
    // Build model matrix: T * Ry * Rx * Rz * S
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    
    // Y-X-Z rotation order: yaw (Y) applied first in world space,
    // then pitch (X), then roll (Z). This lets the character face a
    // direction via rotation.y (yaw) independently of the coordinate-
    // system correction in rotation.x (pitch).
    m = glm::rotate(m, rotation.y, glm::vec3(0, 1, 0));
    m = glm::rotate(m, rotation.x, glm::vec3(1, 0, 0));
    m = glm::rotate(m, rotation.z, glm::vec3(0, 0, 1));
    m = glm::scale(m, scale);
    return m;
  }
};

// ── Character Movement & Locomotion ──────────────────────────────────────────
struct CharacterComponent {
  glm::vec3 targetPosition{0.0f};           // Destination from right-click command
  float moveSpeed = 6.0f;                   // Units per second (max speed)
  float heightOffset = 0.0f;                // Unused (for terrain-aware height)
  bool hasTarget = false;                   // True when moving toward targetPosition
  
  // Smooth rotation state (quaternion slerp)
  glm::quat currentFacing{1.0f, 0.0f, 0.0f, 0.0f}; 
  
  // Smooth acceleration/deceleration
  float currentSpeed = 0.0f;                // Currently moving at this speed (0 to moveSpeed)
};

// ── Selection System ─────────────────────────────────────────────────────────
struct SelectableComponent {
  bool isSelected = false;                  // Highlighted in green
  float selectionRadius = 1.0f;             // Click detection radius
};

struct UnitComponent {
  enum class State { IDLE, MOVING, ATTACKING };
  State state = State::IDLE;
  glm::vec3 targetPosition{0.0f};           // Same as CharacterComponent.targetPosition
  float moveSpeed = 5.0f;
};

struct SelectionState {
  bool isDragging = false;                  // Marquee selection in progress
  glm::vec2 dragStart{0.0f};                // Screen-space start (pixels)
  glm::vec2 dragEnd{0.0f};                  // Screen-space current (pixels)
};

// ── Skeletal Animation ───────────────────────────────────────────────────────
struct SkeletonComponent {
  Skeleton skeleton;                                              // Joint hierarchy
  std::vector<std::vector<SkinVertex>> skinVertices;             // Per-mesh, per-vertex bone influences
  std::vector<std::vector<Vertex>> bindPoseVertices;             // CPU copy of bind-pose geometry
};

struct AnimationComponent {
  AnimationPlayer player;                   // Playback state, keyframe sampling, crossfade
  std::vector<AnimationClip> clips;         // Loaded animations (e.g., [Idle, Walk])
  int activeClipIndex = -1;                 // Currently playing clip (0=Idle, 1=Walk)
  std::vector<Vertex> skinnedVertices;      // CPU-skinned output (for dynamic mesh)
};

// ── GPU-Resident Skinned Mesh ───────────────────────────────────────────────
struct GPUSkinnedMeshComponent {
  uint32_t staticSkinnedMeshIndex = 0;      // Index into Scene::m_staticSkinnedMeshes
  uint32_t boneSlot = 0;                    // Slot in the ring-buffer bone SSBO (0..MAX_SKINNED_CHARS-1)
};

// ── Dynamic (CPU-Updated) Mesh ──────────────────────────────────────────────
struct DynamicMeshComponent {
  uint32_t dynamicMeshIndex = 0;            // Index into Scene::m_dynamicMeshes
};

} // namespace glory
```

**Component Design Principles:**
- **Separation of concerns**: Movement logic in `CharacterComponent`, rendering in `GPUSkinnedMeshComponent`
- **Ring-buffer bone slots**: Multiple characters can be animated simultaneously; each gets a slice of the shared bone SSBO
- **Smooth state**: `currentFacing` (quaternion) and `currentSpeed` (float) enable frame-rate-independent smoothing
- **Animation-driven state**: `activeClipIndex` switches between idle/walk; crossfade is handled by `AnimationPlayer`

---

## Main Renderer Loop & Movement Update

### Renderer::drawFrame() – Per-Frame Update Sequence

Located in `src/renderer/Renderer.cpp` (lines ~97–438), this is the heart of the frame loop:

```cpp
void Renderer::drawFrame() {
    // ── Timing ──────────────────────────────────────────────────────────────
    float currentTime = static_cast<float>(glfwGetTime());
    float dt = currentTime - m_lastFrameTime;  // Delta time in seconds
    m_lastFrameTime = currentTime;
    m_gameTime += dt;
    
    // ── GPU Sync: Wait for previous frame to finish ──────────────────────────
    VkDevice dev = m_device->getDevice();
    VkFence fence = m_sync->getInFlightFence(m_currentFrame);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);

    // ── Acquire Swapchain Image ─────────────────────────────────────────────
    uint32_t imageIndex = 0;
    VkSemaphore imgSem = m_sync->getImageAvailableSemaphore(m_currentFrame);
    VkResult result = vkAcquireNextImageKHR(dev, m_swapchain->getSwapchain(),
                                             UINT64_MAX, imgSem, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("Failed to acquire swapchain image");

    vkResetFences(dev, 1, &fence);
    
    m_debugRenderer.clear();

    // ── Input Polling ────────────────────────────────────────────────────────
    auto ext = m_swapchain->getExtent();
    float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);

    int winW, winH;
    glfwGetWindowSize(m_window.getHandle(), &winW, &winH);

    double mx, my;
    glfwGetCursorPos(m_window.getHandle(), &mx, &my);

    // ── Isometric Camera Update ──────────────────────────────────────────────
    // Update camera: zoom (scroll), pan (middle-mouse drag), follow (attached mode)
    m_isoCam.update(dt, static_cast<float>(winW), static_cast<float>(winH),
                    mx, my,
                    glfwGetMouseButton(m_window.getHandle(), GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS,
                    m_input->consumeScrollDelta());

    // F4: Toggle debug grid
    if (m_input->wasF4Pressed()) m_showGrid = !m_showGrid;

    // ── RIGHT CLICK: Issue Movement Command ──────────────────────────────────
    if (m_input->wasRightClicked() && m_playerEntity != entt::null) {
        glm::vec2 clickPos = m_input->getLastClickPos();
        // Unproject screen-space click to world-space on Y=0 plane
        glm::vec3 worldPos = screenToWorld(clickPos.x, clickPos.y);
        
        // Set character's movement target
        auto& c = m_scene.getRegistry().get<CharacterComponent>(m_playerEntity);
        c.targetPosition = worldPos;
        c.hasTarget = true;
        
        // Spawn click indicator VFX
        m_clickAnim = ClickAnim{ worldPos, 0.0f, 0.25f };
    }

    // ── Unit Spawning (X Key) ────────────────────────────────────────────────
    // Press X to spawn minions at mouse position for testing
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

        uint32_t flatNorm = 0;
        m_scene.getRegistry().emplace<MaterialComponent>(minion,
            MaterialComponent{ m_minionTexIndex, flatNorm, 0.0f, 0.0f, 0.5f, 0.2f });
        
        // Setup minion's skeleton and animations
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

    // ── Selection System: Marquee Drag ───────────────────────────────────────
    if (m_input->isLeftMouseDown()) {
        if (!m_selection.isDragging) {
            m_selection.isDragging = true;
            m_selection.dragStart = m_input->getMousePos();
        }
        m_selection.dragEnd = m_input->getMousePos();
        
        // Draw marquee box on ground plane
        glm::vec3 tl = screenToWorld(m_selection.dragStart.x, m_selection.dragStart.y);
        glm::vec3 tr = screenToWorld(m_selection.dragEnd.x, m_selection.dragStart.y);
        glm::vec3 br = screenToWorld(m_selection.dragEnd.x, m_selection.dragEnd.y);
        glm::vec3 bl = screenToWorld(m_selection.dragStart.x, m_selection.dragEnd.y);
        
        tl.y = tr.y = br.y = bl.y = 0.05f;  // Offset above ground to prevent z-fighting
        
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
            bool isClick = dist < 5.0f;  // Small drag = click selection

            auto view = m_scene.getRegistry().view<TransformComponent, SelectableComponent>();
            glm::vec2 min = glm::min(start, end);
            glm::vec2 max = glm::max(start, end);

            if (isClick) {
                // Single-click: check if clicking on a unit
                bool clickedOnUnit = false;
                for (auto e : view) {
                    auto& t = view.get<TransformComponent>(e);
                    if (glm::distance(worldToScreen(t.position), end) < 20.0f) {
                        clickedOnUnit = true;
                        break;
                    }
                }

                if (clickedOnUnit) {
                    // Click on unit: select it (with Shift for multi-select)
                    for (auto e : view) {
                        auto& t = view.get<TransformComponent>(e);
                        auto& s = view.get<SelectableComponent>(e);
                        if (glm::distance(worldToScreen(t.position), end) < 20.0f) {
                            s.isSelected = true;
                        } else {
                            if (!glfwGetKey(m_window.getHandle(), GLFW_KEY_LEFT_SHIFT)) 
                                s.isSelected = false;
                        }
                    }
                } else {
                    // Click on ground: move selected units to clicked location
                    glm::vec3 targetWorld = screenToWorld(end.x, end.y);
                    auto unitView = m_scene.getRegistry().view<SelectableComponent, CharacterComponent, UnitComponent>();
                    
                    // Count selected units for formation spacing
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
                            
                            // Circular formation offset
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
                // Marquee selection: select all units in box
                for (auto e : view) {
                    auto& t = view.get<TransformComponent>(e);
                    auto& s = view.get<SelectableComponent>(e);
                    glm::vec2 screenPos = worldToScreen(t.position);
                    if (screenPos.x >= min.x && screenPos.x <= max.x &&
                        screenPos.y >= min.y && screenPos.y <= max.y) {
                        s.isSelected = true;
                    } else {
                        if (!glfwGetKey(m_window.getHandle(), GLFW_KEY_LEFT_SHIFT)) 
                            s.isSelected = false;
                    }
                }
            }
        }
    }

    // ── UPDATE MINIONS MOVEMENT ─────────────────────────────────────────────
    // (Handles all spawned units, not player character)
    auto minionView = m_scene.getRegistry().view<UnitComponent, CharacterComponent, TransformComponent>();
    for (auto e : minionView) {
        if (e == m_playerEntity) continue;  // Skip player
        
        auto& c = minionView.get<CharacterComponent>(e);
        auto& t = minionView.get<TransformComponent>(e);
        auto& u = minionView.get<UnitComponent>(e);

        const float turnSpeed = 20.0f;   // rad/s (~1145°/s, MOBA-snappy)
        const float accelRate = 10.0f;   // 0→full speed in ~0.1s

        if (c.hasTarget) {
            glm::vec3 dir = c.targetPosition - t.position;
            dir.y = 0.0f;  // Ignore height
            float dist = glm::length(dir);
            
            if (dist < 0.1f) {
                // Reached target
                c.hasTarget = false;
                u.state = UnitComponent::State::IDLE;
            } else {
                // Move toward target
                dir /= dist;
                
                // Smooth rotation via quaternion slerp (see next section)
                glm::quat targetFacing = glm::angleAxis(std::atan2(dir.x, dir.z), glm::vec3(0.0f, 1.0f, 0.0f));
                c.currentFacing = glm::slerp(c.currentFacing, targetFacing, std::min(turnSpeed * dt, 1.0f));
                t.rotation.y = glm::eulerAngles(c.currentFacing).y;
                
                // Smooth acceleration
                c.currentSpeed += (c.moveSpeed - c.currentSpeed) * std::min(accelRate * dt, 1.0f);
                t.position += dir * c.currentSpeed * dt;
                u.state = UnitComponent::State::MOVING;
            }
        } else {
            // Decelerate to stop
            c.currentSpeed += (0.0f - c.currentSpeed) * std::min(accelRate * dt, 1.0f);
        }
    }

    // ── UPDATE PLAYER CHARACTER MOVEMENT ─────────────────────────────────────
    if (m_playerEntity != entt::null &&
        m_scene.getRegistry().valid(m_playerEntity) &&
        m_scene.getRegistry().all_of<CharacterComponent, TransformComponent>(m_playerEntity)) {

        const float turnSpeed = 20.0f;
        const float accelRate = 10.0f;

        auto& c = m_scene.getRegistry().get<CharacterComponent>(m_playerEntity);
        auto& t = m_scene.getRegistry().get<TransformComponent>(m_playerEntity);
        
        if (c.hasTarget) {
            glm::vec3 dir = c.targetPosition - t.position;
            dir.y = 0.0f;
            float dist = glm::length(dir);
            
            if (dist < 0.1f) {
                c.hasTarget = false;
            } else {
                dir /= dist;
                
                // Same smooth rotation and acceleration as minions
                glm::quat targetFacing = glm::angleAxis(std::atan2(dir.x, dir.z), glm::vec3(0.0f, 1.0f, 0.0f));
                c.currentFacing = glm::slerp(c.currentFacing, targetFacing, std::min(turnSpeed * dt, 1.0f));
                t.rotation.y = glm::eulerAngles(c.currentFacing).y;
                
                c.currentSpeed += (c.moveSpeed - c.currentSpeed) * std::min(accelRate * dt, 1.0f);
                t.position += dir * c.currentSpeed * dt;
            }
        } else {
            c.currentSpeed += (0.0f - c.currentSpeed) * std::min(accelRate * dt, 1.0f);
        }
        
        // Update camera follow target
        m_isoCam.setFollowTarget(t.position);
    }

    // ── UPDATE ANIMATIONS ────────────────────────────────────────────────────
    // Switch idle/walk animations based on movement state
    auto animView = m_scene.getRegistry()
        .view<SkeletonComponent, AnimationComponent, GPUSkinnedMeshComponent, TransformComponent>();
    uint32_t currentBoneSlot = 0;
    
    for (auto&& [e, skel, anim, ssm, t] : animView.each()) {
        // Determine target animation clip
        if (m_scene.getRegistry().all_of<CharacterComponent>(e)) {
            auto& c = m_scene.getRegistry().get<CharacterComponent>(e);
            
            // 0=Idle, 1=Walk (or whatever clips were loaded)
            int targetClip = c.hasTarget ? 1 : 0;
            
            // Crossfade to target clip if different
            if (anim.activeClipIndex != targetClip && targetClip < (int)anim.clips.size()) {
                anim.activeClipIndex = targetClip;
                anim.player.crossfadeTo(&anim.clips[targetClip], 0.15f);
            }
            
            // FOOT SLIDE PREVENTION: Scale walk animation speed to match movement
            // Skip during crossfade so incoming clip plays normally
            if (anim.activeClipIndex == 1 && c.moveSpeed > 0.0f && !anim.player.isBlending()) {
                anim.player.setTimeScale(c.currentSpeed / c.moveSpeed);
            } else {
                anim.player.setTimeScale(1.0f);
            }
        }
        
        // Update animation playback
        anim.player.refreshSkeleton(&skel.skeleton);
        anim.player.update(dt);
        
        // Write skinning matrices to GPU ring-buffer
        const auto& matrices = anim.player.getSkinningMatrices();
        m_descriptors->writeBoneSlot(m_currentFrame, currentBoneSlot, matrices);
        ssm.boneSlot = currentBoneSlot++;
    }

    // ── Click Animation VFX ──────────────────────────────────────────────────
    if (m_clickAnim) {
        m_clickAnim->lifetime += dt;
        if (m_clickAnim->lifetime >= m_clickAnim->maxLife)
            m_clickAnim.reset();
    }

    // ── Record and Submit Command Buffer ─────────────────────────────────────
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

    // ── Present Swapchain ────────────────────────────────────────────────────
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
```

**Key Frame Loop Insights:**
1. **Right-click → Movement**: Query input, unproject to world, set `hasTarget = true`, start smooth rotation/acceleration
2. **Selection + Formation**: Marquee or click selection, circular offset per unit
3. **Movement Update**: Smooth rotation (quaternion slerp), smooth acceleration, position update
4. **Animation State Switch**: `hasTarget ? Walk : Idle` with 0.15s crossfade
5. **Foot Slide Fix**: Scale walk animation speed to match actual `currentSpeed` to prevent sliding
6. **GPU Sync**: Write bone matrices to ring-buffer before rendering each frame

---

## Rotation & Smooth Facing

### Quaternion-Based Smooth Rotation

The character rotation system uses **quaternion spherical linear interpolation (slerp)** to achieve smooth, frame-rate-independent turning:

```cpp
// From Renderer::drawFrame() and minion update loops:

const float turnSpeed = 20.0f;  // radians per second (~1145°/s)

// 1. Calculate direction to target
glm::vec3 dir = c.targetPosition - t.position;
dir.y = 0.0f;  // Flatten to XZ plane
float dist = glm::length(dir);

if (dist > 0.1f) {
    dir /= dist;  // Normalize
    
    // 2. Convert direction vector to quaternion (rotation around Y-axis)
    // std::atan2(x, z) gives yaw angle from direction vector
    glm::quat targetFacing = glm::angleAxis(std::atan2(dir.x, dir.z), glm::vec3(0.0f, 1.0f, 0.0f));
    
    // 3. Spherical linear interpolation
    // Clamp turnSpeed * dt to [0, 1] for smooth blending
    float alpha = std::min(turnSpeed * dt, 1.0f);
    c.currentFacing = glm::slerp(c.currentFacing, targetFacing, alpha);
    
    // 4. Convert quaternion back to Euler angle for transform
    t.rotation.y = glm::eulerAngles(c.currentFacing).y;
}
```

**Why This Approach:**
- **Quaternion slerp** avoids gimbal lock and provides smooth shortest-path rotation
- **Clamped alpha**: `std::min(turnSpeed * dt, 1.0f)` prevents overshooting if FPS drops
- **Smooth acceleration + smooth rotation**: Together create fluid, non-snappy (MOBA-style) locomotion

**Example Timeline** (assuming 60 FPS, turnSpeed=20 rad/s):
- Frame 1: dt=0.0167s, alpha=0.334, slerp rotates ~33.4% toward target
- Frame 2: dt=0.0167s, alpha=0.334, rotates ~33.4% more (total ~56%)
- Frame 3: dt=0.0167s, alpha=0.334, reaches target direction

---

## Animation State Machine & Crossfading

### Animation Clip Structure

Located in `src/animation/AnimationClip.h`:

```cpp
enum class AnimationPath { Translation, Rotation, Scale };

struct AnimationChannel {
  int targetJointIndex = -1;               // Which joint this channel animates
  AnimationPath path = AnimationPath::Translation;

  std::vector<float> timestamps;           // Keyframe times (seconds)

  // Only one of these is populated depending on path type
  std::vector<glm::vec3> translationKeys;  // Position keyframes
  std::vector<glm::quat> rotationKeys;     // Rotation keyframes (GLM order: w,x,y,z)
  std::vector<glm::vec3> scaleKeys;        // Scale keyframes
};

struct AnimationClip {
  std::string name;                        // "Idle", "Walk", etc.
  float duration = 0.0f;                   // Total length in seconds
  bool looping = true;                     // Whether animation loops
  std::vector<AnimationChannel> channels;  // Channels for each joint

  // Optional: the rest pose this clip was authored against.
  // When retargeting (loading idle.glb's skeleton onto walk.glb),
  // use these instead of target skeleton's rest pose.
  std::vector<ClipRestPose> restPose;
};
```

### Animation Player – Playback & Crossfading

From `src/animation/AnimationPlayer.h`:

```cpp
class AnimationPlayer {
public:
  void setSkeleton(const Skeleton *skeleton);
  void setClip(const AnimationClip *clip);

  // Smoothly blend from current clip to newClip over blendDuration seconds.
  void crossfadeTo(const AnimationClip *newClip, float blendDuration = 0.15f);

  void update(float deltaTime);

  // Scale animation playback speed (1.0 = normal, 0.5 = half speed, etc.)
  void setTimeScale(float scale) { m_timeScale = scale; }

  // Returns globalTransform[i] * inverseBindMatrix[i] for each joint
  const std::vector<glm::mat4> &getSkinningMatrices() const {
    return m_skinningMatrices;
  }

  bool isPlaying() const { return m_clip != nullptr; }
  bool isBlending() const { return m_isBlending; }

private:
  const Skeleton *m_skeleton = nullptr;
  const AnimationClip *m_clip = nullptr;
  float m_time = 0.0f;                     // Current playback time
  float m_timeScale = 1.0f;                // Playback speed multiplier

  // Crossfade blend state
  const AnimationClip *m_prevClip = nullptr;
  float m_blendTime = 0.0f;
  float m_blendDuration = 0.0f;
  bool m_isBlending = false;

  std::vector<JointPose> m_localPoses;     // Current joint poses
  std::vector<JointPose> m_prevPoses;      // Frozen snapshot for blend source
  std::vector<glm::mat4> m_globalTransforms;  // World-space joint transforms
  std::vector<glm::mat4> m_skinningMatrices;  // Final matrices for GPU/CPU skinning
};
```

### Animation Player Implementation

From `src/animation/AnimationPlayer.cpp` (lines ~107–171):

```cpp
void AnimationPlayer::crossfadeTo(const AnimationClip *newClip, float blendDuration) {
  if (newClip == m_clip)
    return;

  // Snapshot current pose as the blend source
  m_prevClip = m_clip;
  m_prevPoses = m_localPoses;  // Freeze the current pose

  // Switch to new clip
  m_clip = newClip;
  m_time = 0.0f;               // Reset playback time
  m_blendTime = 0.0f;
  m_blendDuration = blendDuration;
  m_isBlending = (m_prevClip != nullptr && blendDuration > 0.0f);
}

void AnimationPlayer::update(float deltaTime) {
  if (!m_skeleton)
    return;

  // Advance blend timer
  if (m_isBlending) {
    m_blendTime += deltaTime;
    if (m_blendTime >= m_blendDuration) {
      m_isBlending = false;
      m_prevClip = nullptr;  // Stop referencing old clip
    }
  }

  // Advance and sample current clip
  if (m_clip && m_clip->duration > 0.0f) {
    m_time += deltaTime * m_timeScale;  // FOOT SLIDE FIX: timeScale changes here

    if (m_clip->looping) {
      if (m_time > m_clip->duration) {
        m_time = std::fmod(m_time, m_clip->duration);  // Wrap around
      }
    } else {
      m_time = std::min(m_time, m_clip->duration);  // Clamp to end
    }
    sampleInto(m_clip, m_time, m_localPoses);
  }

  // If blending, interpolate from frozen snapshot to current pose
  // We do NOT re-sample the previous clip — keeping it frozen avoids
  // jump artifacts when the previous animation loops back during blend
  if (m_isBlending && m_prevPoses.size() == m_localPoses.size()) {
    float blendFactor = std::clamp(m_blendTime / m_blendDuration, 0.0f, 1.0f);
    
    for (size_t i = 0; i < m_localPoses.size(); ++i) {
      // Linear interpolation for translation and scale
      m_localPoses[i].translation =
          glm::mix(m_prevPoses[i].translation, m_localPoses[i].translation, blendFactor);
      
      // Spherical linear interpolation for rotation
      // (shortest-path: negate q2 if dot product is negative)
      glm::quat q1 = m_prevPoses[i].rotation;
      glm::quat q2 = m_localPoses[i].rotation;
      if (glm::dot(q1, q2) < 0.0f)
        q2 = -q2;  // Take shorter path around hypersphere
      m_localPoses[i].rotation = glm::normalize(glm::slerp(q1, q2, blendFactor));
      
      m_localPoses[i].scale =
          glm::mix(m_prevPoses[i].scale, m_localPoses[i].scale, blendFactor);
    }
  }

  computeGlobalTransforms();  // Build world-space joint matrices
}
```

### Keyframe Sampling

From `src/animation/AnimationPlayer.cpp` (lines ~174–268):

```cpp
// Binary search to find which keyframe interval contains time t
static size_t findKeyframe(const std::vector<float> &timestamps, float t, bool looping) {
  if (timestamps.size() <= 1)
    return 0;

  if (looping) {
    if (t >= timestamps.back())
      return timestamps.size() - 1;  // Wrap-around interval
  } else {
    if (t <= timestamps.front())
      return 0;
    if (t >= timestamps.back())
      return timestamps.size() - 2;
  }

  auto it = std::upper_bound(timestamps.begin(), timestamps.end(), t);
  size_t idx = static_cast<size_t>(it - timestamps.begin());
  return idx > 0 ? idx - 1 : 0;
}

void AnimationPlayer::sampleInto(const AnimationClip *clip, float t,
                                 std::vector<JointPose> &poses) {
  if (!clip || !m_skeleton)
    return;

  // Reset to rest pose (either clip's rest or skeleton's rest)
  bool hasClipRest = !clip->restPose.empty() &&
                     clip->restPose.size() == m_skeleton->joints.size();
  for (size_t i = 0; i < m_skeleton->joints.size(); ++i) {
    if (hasClipRest) {
      poses[i].translation = clip->restPose[i].translation;
      poses[i].rotation    = clip->restPose[i].rotation;
      poses[i].scale       = clip->restPose[i].scale;
    } else {
      poses[i].translation = m_skeleton->joints[i].localTranslation;
      poses[i].rotation    = m_skeleton->joints[i].localRotation;
      poses[i].scale       = m_skeleton->joints[i].localScale;
    }
  }

  // Sample each animation channel
  for (const auto &channel : clip->channels) {
    if (channel.targetJointIndex < 0 ||
        channel.targetJointIndex >= static_cast<int>(poses.size()))
      continue;

    auto &pose = poses[channel.targetJointIndex];

    if (channel.timestamps.empty())
      continue;

    // Find surrounding keyframes
    size_t i0 = findKeyframe(channel.timestamps, t, clip->looping);
    size_t i1 = (i0 + 1) % channel.timestamps.size();

    float t0 = channel.timestamps[i0];
    float t1 = channel.timestamps[i1];

    if (i1 == 0 && clip->looping) {
      t1 = clip->duration;  // Wrap-around: extrapolate to loop duration
    }

    // Linear interpolation parameter [0, 1]
    float alpha = (i0 == i1) ? 0.0f : (t - t0) / (t1 - t0);
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    // Interpolate based on channel type
    switch (channel.path) {
    case AnimationPath::Translation:
      if (i0 < channel.translationKeys.size() &&
          i1 < channel.translationKeys.size()) {
        pose.translation =
            glm::mix(channel.translationKeys[i0],
                     channel.translationKeys[i1], alpha);
      }
      break;
    case AnimationPath::Rotation:
      if (i0 < channel.rotationKeys.size() &&
          i1 < channel.rotationKeys.size()) {
        pose.rotation = glm::normalize(
            glm::slerp(channel.rotationKeys[i0],
                       channel.rotationKeys[i1], alpha));
      }
      break;
    case AnimationPath::Scale:
      if (i0 < channel.scaleKeys.size() && i1 < channel.scaleKeys.size()) {
        pose.scale = glm::mix(channel.scaleKeys[i0],
                              channel.scaleKeys[i1], alpha);
      }
      break;
    }
  }
}

void AnimationPlayer::computeGlobalTransforms() {
  if (!m_skeleton)
    return;

  size_t n = m_skeleton->joints.size();

  // Walk joints in topological order (parents first, guaranteed by GLB loader)
  for (size_t i = 0; i < n; ++i) {
    const auto &pose = m_localPoses[i];

    // Build local TRS matrix
    glm::mat4 T = glm::translate(glm::mat4(1.0f), pose.translation);
    glm::mat4 R = glm::toMat4(pose.rotation);
    glm::mat4 S = glm::scale(glm::mat4(1.0f), pose.scale);
    glm::mat4 localMat = T * R * S;

    int parent = m_skeleton->joints[i].parentIndex;
    if (parent >= 0 && parent < static_cast<int>(n)) {
      // Child: multiply by parent's global transform
      m_globalTransforms[i] = m_globalTransforms[parent] * localMat;
    } else {
      // Root: include armature transform (handles Z-up→Y-up, scale, etc.)
      m_globalTransforms[i] = m_skeleton->armatureTransform * localMat;
    }

    // Skinning matrix = globalTransform * inverseBindMatrix
    m_skinningMatrices[i] =
        m_globalTransforms[i] * m_skeleton->joints[i].inverseBindMatrix;
  }
}
```

**Animation State Machine Logic** (in Renderer::drawFrame, lines ~368–393):

```cpp
// Switch clip based on movement state with crossfade
if (m_scene.getRegistry().all_of<CharacterComponent>(e)) {
    auto& c = m_scene.getRegistry().get<CharacterComponent>(e);
    int targetClip = c.hasTarget ? 1 : 0;  // 1=Walk, 0=Idle
    
    if (anim.activeClipIndex != targetClip && targetClip < (int)anim.clips.size()) {
        anim.activeClipIndex = targetClip;
        anim.player.crossfadeTo(&anim.clips[targetClip], 0.15f);  // 150ms blend
    }
    
    // FOOT SLIDE PREVENTION
    if (anim.activeClipIndex == 1 && c.moveSpeed > 0.0f && !anim.player.isBlending()) {
        // Scale walk animation speed to match actual movement speed
        anim.player.setTimeScale(c.currentSpeed / c.moveSpeed);
    } else {
        anim.player.setTimeScale(1.0f);
    }
}
```

**Key Insights:**
1. **Crossfade**: Freezes old pose, samples new animation from frame 0, blends over 0.15s
2. **Frozen snapshot**: Prevents looping artifacts; `m_prevClip` never re-sampled during blend
3. **Foot slide fix**: When walk animation plays, its duration is scaled to match actual movement (if animationDuration = 1s but character moves in 0.5s, play at 2x speed)
4. **State switch**: `hasTarget` drives clip index; smooth acceleration prevents jerky transitions

---

## Skeletal Animation Pipeline

### Skeleton Hierarchy

From `src/animation/Skeleton.h`:

```cpp
struct Joint {
  std::string name;
  int parentIndex = -1;  // -1 = root joint
  glm::mat4 inverseBindMatrix{1.0f};  // From GLB skin definition
  glm::vec3 localTranslation{0.0f};   // Local rest pose (relative to parent)
  glm::quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};  // GLM order: w,x,y,z
  glm::vec3 localScale{1.0f};
};

struct Skeleton {
  std::vector<Joint> joints;
  std::vector<int> jointNodeIndices;  // glTF node index per joint

  // World-space transform of armature root (parent of all joints).
  // Extracted from GLB scene graph; includes Z-up→Y-up rotation + scale.
  glm::mat4 armatureTransform{1.0f};

  int findJoint(const std::string &name) const {
    for (size_t i = 0; i < joints.size(); ++i) {
      if (joints[i].name == name)
        return static_cast<int>(i);
    }
    return -1;
  }
};

struct SkinVertex {
  glm::ivec4 joints{0};    // Joint indices (0-3)
  glm::vec4 weights{0.0f}; // Weights (sum to 1.0)
};
```

### GLB Loader – Skeleton Extraction

From `src/renderer/GLBLoader.cpp` (lines ~313–512):

The GLB loader extracts skeleton from glTF `skin[0]`:

```cpp
// ══════════════════════════════════════════════════════════════════════════
// 1. Extract skeleton from skin[0]
// ══════════════════════════════════════════════════════════════════════════
if (gltfModel.skins.empty())
  throw std::runtime_error("GLB has no skin data: " + filepath);

const auto &skin = gltfModel.skins[0];
int jointCount = static_cast<int>(skin.joints.size());
spdlog::info("Skinned GLB '{}': {} joints", filepath, jointCount);

// Build node→parent map from scene graph
std::vector<int> nodeParent;
buildParentMap(gltfModel, nodeParent);

// Map glTF node index → joint index in skeleton
std::unordered_map<int, int> nodeToJoint;
for (int j = 0; j < jointCount; ++j) {
  nodeToJoint[skin.joints[j]] = j;
}

// Read inverse bind matrices from GLB accessors
std::vector<glm::mat4> inverseBindMatrices(jointCount, glm::mat4(1.0f));
if (skin.inverseBindMatrices >= 0) {
  const auto *ibmData = reinterpret_cast<const float *>(
      accessorData(gltfModel, skin.inverseBindMatrices));
  for (int j = 0; j < jointCount; ++j) {
    std::memcpy(&inverseBindMatrices[j], ibmData + j * 16,
                sizeof(float) * 16);
  }
}

// ── Extract armature root transform ────────────────────────────────────
// The armature node sits above skeleton root in scene graph.
// It typically contains Z-up→Y-up rotation + unit scale from Blender.
{
  int armatureNode = -1;
  if (!skin.joints.empty()) {
    int firstJointNode = skin.joints[0];
    int cursor = nodeParent[firstJointNode];
    while (cursor >= 0) {
      if (nodeToJoint.count(cursor) == 0) {
        armatureNode = cursor;  // Found non-joint ancestor
        break;
      }
      cursor = nodeParent[cursor];
    }
  }
  
  if (armatureNode >= 0) {
    const auto &aNode = gltfModel.nodes[armatureNode];
    glm::vec3 aT(0.0f), aS(1.0f);
    glm::quat aR(1.0f, 0.0f, 0.0f, 0.0f);
    
    if (aNode.translation.size() == 3) {
      aT = glm::vec3(aNode.translation[0], aNode.translation[1], aNode.translation[2]);
    }
    if (aNode.rotation.size() == 4) {
      // glTF: (x,y,z,w) → GLM: (w,x,y,z)
      aR = glm::quat(aNode.rotation[3], aNode.rotation[0], 
                     aNode.rotation[1], aNode.rotation[2]);
    }
    if (aNode.scale.size() == 3) {
      aS = glm::vec3(aNode.scale[0], aNode.scale[1], aNode.scale[2]);
    }
    
    result.skeleton.armatureTransform = glm::translate(glm::mat4(1.0f), aT) * 
                                        glm::toMat4(aR) * 
                                        glm::scale(glm::mat4(1.0f), aS);
  }
}

// Build joints with parent indices and rest-pose transforms
result.skeleton.joints.resize(jointCount);
result.skeleton.jointNodeIndices.resize(jointCount);

for (int j = 0; j < jointCount; ++j) {
  int nodeIdx = skin.joints[j];
  const auto &node = gltfModel.nodes[nodeIdx];
  auto &joint = result.skeleton.joints[j];

  joint.name = node.name;
  joint.inverseBindMatrix = inverseBindMatrices[j];
  result.skeleton.jointNodeIndices[j] = nodeIdx;

  // Find parent joint by walking up glTF node tree
  int parentNode = nodeParent[nodeIdx];
  joint.parentIndex = -1;
  while (parentNode >= 0) {
    auto it = nodeToJoint.find(parentNode);
    if (it != nodeToJoint.end()) {
      joint.parentIndex = it->second;
      break;
    }
    parentNode = nodeParent[parentNode];
  }

  // Extract rest pose from node TRS
  if (node.translation.size() == 3) {
    joint.localTranslation = glm::vec3(node.translation[0], 
                                       node.translation[1], 
                                       node.translation[2]);
  }
  if (node.rotation.size() == 4) {
    joint.localRotation = glm::quat(node.rotation[3], node.rotation[0],
                                    node.rotation[1], node.rotation[2]);
  }
  if (node.scale.size() == 3) {
    joint.localScale = glm::vec3(node.scale[0], node.scale[1], node.scale[2]);
  }
}

// Topological sort: ensure parents come before children
// (Required for correct global transform computation)
{
  std::vector<Joint> sorted(jointCount);
  std::vector<int> sortedNodeIndices(jointCount);
  std::vector<int> oldToNew(jointCount, -1);
  std::vector<bool> placed(jointCount, false);
  int outIdx = 0;

  while (outIdx < jointCount) {
    bool progress = false;
    for (int j = 0; j < jointCount; ++j) {
      if (placed[j]) continue;
      int parent = result.skeleton.joints[j].parentIndex;
      if (parent < 0 || placed[parent]) {
        oldToNew[j] = outIdx;
        sorted[outIdx] = result.skeleton.joints[j];
        sortedNodeIndices[outIdx] = result.skeleton.jointNodeIndices[j];
        placed[j] = true;
        ++outIdx;
        progress = true;
      }
    }
    if (!progress) break;  // Bail on cycles
  }

  // Remap parent indices after sort
  for (int j = 0; j < jointCount; ++j) {
    int oldParent = sorted[j].parentIndex;
    sorted[j].parentIndex = (oldParent >= 0) ? oldToNew[oldParent] : -1;
  }

  result.skeleton.joints = std::move(sorted);
  result.skeleton.jointNodeIndices = std::move(sortedNodeIndices);
  nodeToJoint.clear();
  for (int j = 0; j < jointCount; ++j) {
    nodeToJoint[result.skeleton.jointNodeIndices[j]] = j;
  }
}
```

### Mesh & Skin Vertex Extraction

From `src/renderer/GLBLoader.cpp` (lines ~516–622):

```cpp
// ══════════════════════════════════════════════════════════════════════════
// 2. Extract meshes with JOINTS_0 / WEIGHTS_0
// ══════════════════════════════════════════════════════════════════════════
for (const auto &mesh : gltfModel.meshes) {
  for (const auto &prim : mesh.primitives) {
    // Get position data (required)
    auto posIt = prim.attributes.find("POSITION");
    if (posIt == prim.attributes.end())
      continue;

    const size_t vertCount = accessorCount(gltfModel, posIt->second);
    size_t posStride = getAccessorStride(gltfModel, posIt->second, sizeof(float) * 3);
    const uint8_t *posBytes = accessorData(gltfModel, posIt->second);

    // Extract normals (optional)
    const uint8_t *normRaw = nullptr;
    size_t normStride = sizeof(float) * 3;
    auto normIt = prim.attributes.find("NORMAL");
    if (normIt != prim.attributes.end()) {
      normRaw = accessorData(gltfModel, normIt->second);
      normStride = getAccessorStride(gltfModel, normIt->second, sizeof(float) * 3);
    }

    // Extract UVs (optional, with component type handling)
    const uint8_t *uvRaw = nullptr;
    size_t uvStride = sizeof(float) * 2;
    int uvComponentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    auto uvIt = prim.attributes.find("TEXCOORD_0");
    if (uvIt != prim.attributes.end()) {
      uvRaw = accessorData(gltfModel, uvIt->second);
      uvStride = getAccessorStride(gltfModel, uvIt->second, sizeof(float) * 2);
      uvComponentType = gltfModel.accessors[uvIt->second].componentType;
    }

    // Build vertex array
    std::vector<Vertex> vertices(vertCount);
    for (size_t i = 0; i < vertCount; ++i) {
      auto &v = vertices[i];
      std::memcpy(&v.position, posBytes + i * posStride, sizeof(float) * 3);
      v.color = glm::vec3(1.0f);
      if (normRaw)
        std::memcpy(&v.normal, normRaw + i * normStride, sizeof(float) * 3);
      else
        v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
      v.texCoord = decodeUV(i);  // Handles float, uint16, uint8 packing
    }

    // ── JOINTS_0 and WEIGHTS_0 (bone influences) ────────────────────────────
    std::vector<SkinVertex> skinVerts(vertCount);
    auto jointsIt = prim.attributes.find("JOINTS_0");
    auto weightsIt = prim.attributes.find("WEIGHTS_0");

    if (jointsIt != prim.attributes.end() && weightsIt != prim.attributes.end()) {
      const auto &jointsAcc = gltfModel.accessors[jointsIt->second];
      const uint8_t *jointsRaw = accessorData(gltfModel, jointsIt->second);
      const auto &jointsBV = gltfModel.bufferViews[jointsAcc.bufferView];

      const auto &weightsAcc = gltfModel.accessors[weightsIt->second];
      const uint8_t *weightsRaw = accessorData(gltfModel, weightsIt->second);
      const auto &weightsBV = gltfModel.bufferViews[weightsAcc.bufferView];

      for (size_t i = 0; i < vertCount; ++i) {
        auto &sv = skinVerts[i];

        // Read joint indices (may be uint8 or uint16)
        if (jointsAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
          size_t stride = jointsBV.byteStride > 0 ? jointsBV.byteStride : 4;
          const uint8_t *ptr = jointsRaw + i * stride;
          sv.joints = glm::ivec4(ptr[0], ptr[1], ptr[2], ptr[3]);
        } else if (jointsAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
          size_t stride = jointsBV.byteStride > 0 ? jointsBV.byteStride : 8;
          const uint16_t *ptr = reinterpret_cast<const uint16_t *>(jointsRaw + i * stride);
          sv.joints = glm::ivec4(ptr[0], ptr[1], ptr[2], ptr[3]);
        }

        // Read weights (always float)
        size_t wStride = weightsBV.byteStride > 0 ? weightsBV.byteStride : 16;
        std::memcpy(&sv.weights, weightsRaw + i * wStride, sizeof(float) * 4);

        // Normalize weights to ensure they sum to 1.0
        float sum = sv.weights.x + sv.weights.y + sv.weights.z + sv.weights.w;
        if (sum > 0.0f && std::abs(sum - 1.0f) > 1e-4f) {
          sv.weights /= sum;
        }
      }
    }

    // Store for later GPU upload
    result.bindPoseVertices.push_back(vertices);
    result.skinVertices.push_back(std::move(skinVerts));
    result.indices.push_back(indices);
    result.model.m_meshes.emplace_back(device, allocator, vertices, indices);
    result.model.m_meshMaterialIndices.push_back(prim.material);
  }
}
```

### Animation Extraction

From `src/renderer/GLBLoader.cpp` (lines ~724–802):

```cpp
// ══════════════════════════════════════════════════════════════════════════
// 3. Extract animations
// ══════════════════════════════════════════════════════════════════════════
for (const auto &anim : gltfModel.animations) {
  AnimationClip clip;
  clip.name = anim.name;
  clip.duration = 0.0f;
  clip.looping = true;

  // Extract each animation channel (tracks joint animation)
  for (const auto &channel : anim.channels) {
    if (channel.target_node < 0)
      continue;

    // Map glTF node to our skeleton index
    auto it = nodeToJoint.find(channel.target_node);
    if (it == nodeToJoint.end())
      continue;

    const auto &sampler = anim.samplers[channel.sampler];

    AnimationChannel ac;
    ac.targetJointIndex = it->second;

    // Read timestamps (shared between translation/rotation/scale for this joint)
    size_t keyCount = accessorCount(gltfModel, sampler.input);
    const float *timeData = reinterpret_cast<const float *>(
        accessorData(gltfModel, sampler.input));
    ac.timestamps.resize(keyCount);
    std::memcpy(ac.timestamps.data(), timeData, keyCount * sizeof(float));

    if (keyCount > 0 && ac.timestamps.back() > clip.duration)
      clip.duration = ac.timestamps.back();

    // Read keyframe values
    const uint8_t *valData = accessorData(gltfModel, sampler.output);

    if (channel.target_path == "translation") {
      ac.path = AnimationPath::Translation;
      ac.translationKeys.resize(keyCount);
      for (size_t k = 0; k < keyCount; ++k) {
        std::memcpy(&ac.translationKeys[k], valData + k * sizeof(float) * 3,
                    sizeof(float) * 3);
      }
    } else if (channel.target_path == "rotation") {
      ac.path = AnimationPath::Rotation;
      ac.rotationKeys.resize(keyCount);
      for (size_t k = 0; k < keyCount; ++k) {
        // glTF quaternion: (x,y,z,w) → GLM: (w,x,y,z)
        float xyzw[4];
        std::memcpy(xyzw, valData + k * sizeof(float) * 4, sizeof(float) * 4);
        ac.rotationKeys[k] = glm::quat(xyzw[3], xyzw[0], xyzw[1], xyzw[2]);
      }
    } else if (channel.target_path == "scale") {
      ac.path = AnimationPath::Scale;
      ac.scaleKeys.resize(keyCount);
      for (size_t k = 0; k < keyCount; ++k) {
        std::memcpy(&ac.scaleKeys[k], valData + k * sizeof(float) * 3,
                    sizeof(float) * 3);
      }
    }

    clip.channels.push_back(std::move(ac));
  }

  // Store the rest pose this clip was authored against
  // (used for retargeting when loading idle animation onto walk skeleton)
  clip.restPose.resize(jointCount);
  for (int j = 0; j < jointCount; ++j) {
    clip.restPose[j].translation = result.skeleton.joints[j].localTranslation;
    clip.restPose[j].rotation    = result.skeleton.joints[j].localRotation;
    clip.restPose[j].scale       = result.skeleton.joints[j].localScale;
  }

  spdlog::info("Animation '{}': duration={:.3f}s, {} channels", clip.name,
               clip.duration, clip.channels.size());
  result.animations.push_back(std::move(clip));
}

spdlog::info("Skinned GLB loaded: {} meshes, {} joints, {} animations",
             result.model.getMeshCount(), jointCount, result.animations.size());
```

---

## GPU Skinning

### GPU Skinned Mesh

From `src/renderer/StaticSkinnedMesh.h` and `.cpp`:

```cpp
// StaticSkinnedMesh.h
class StaticSkinnedMesh {
public:
    StaticSkinnedMesh(const Device& device, VmaAllocator allocator,
                      const std::vector<SkinnedVertex>& vertices,
                      const std::vector<uint32_t>& indices);

    // Bind vertex + index buffers, then draw
    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    uint32_t getIndexCount()  const { return m_indexCount; }
    uint32_t getVertexCount() const { return m_vertexCount; }

private:
    Buffer   m_vertexBuffer;  // Device-local bind-pose geometry + joint/weight data
    Buffer   m_indexBuffer;   // Device-local indices
    uint32_t m_indexCount  = 0;
    uint32_t m_vertexCount = 0;
};

// StaticSkinnedMesh.cpp
StaticSkinnedMesh::StaticSkinnedMesh(const Device& device, VmaAllocator allocator,
                                     const std::vector<SkinnedVertex>& vertices,
                                     const std::vector<uint32_t>& indices)
{
    m_vertexCount = static_cast<uint32_t>(vertices.size());
    m_indexCount  = static_cast<uint32_t>(indices.size());

    // Upload bind-pose geometry (never changes) to device-local memory
    m_vertexBuffer = Buffer::createDeviceLocal(
        device, allocator,
        vertices.data(), sizeof(SkinnedVertex) * m_vertexCount,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    m_indexBuffer = Buffer::createDeviceLocal(
        device, allocator,
        indices.data(), sizeof(uint32_t) * m_indexCount,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    spdlog::info("StaticSkinnedMesh created: {} vertices, {} indices",
                 m_vertexCount, m_indexCount);
}

void StaticSkinnedMesh::bind(VkCommandBuffer cmd) const {
    VkBuffer     buf     = m_vertexBuffer.getBuffer();
    VkDeviceSize offset  = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.getBuffer(), 0, VK_INDEX_TYPE_UINT32);
}

void StaticSkinnedMesh::draw(VkCommandBuffer cmd) const {
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}
```

### Bone SSBO Management

From `src/renderer/Descriptors.h` and `.cpp`:

```cpp
// Descriptors.h
class Descriptors {
public:
    static constexpr uint32_t MAX_BONES = 256;              // Max bones per character
    static constexpr uint32_t MAX_SKINNED_CHARS = 128;      // Max simultaneous characters

    // Ring-buffer API: write bone matrices for a specific character slot
    uint32_t writeBoneSlot(uint32_t frameIndex, uint32_t slotIndex,
                           const std::vector<glm::mat4>& matrices);

    VkBuffer getBoneBuffer(uint32_t frameIndex) const {
        return m_boneBuffers[frameIndex].getBuffer();
    }

private:
    // Per-frame bone matrix SSBOs: total size = MAX_BONES * MAX_SKINNED_CHARS * sizeof(mat4)
    // Layout: [Char0_Bone0, Char0_Bone1, ..., Char0_Bone255, Char1_Bone0, ...]
    std::vector<Buffer> m_boneBuffers;
};

// Descriptors.cpp
uint32_t Descriptors::writeBoneSlot(uint32_t frameIndex, uint32_t slotIndex,
                                     const std::vector<glm::mat4>& matrices) {
    uint32_t slot = std::min(slotIndex, MAX_SKINNED_CHARS - 1);
    uint32_t count = std::min(static_cast<uint32_t>(matrices.size()), MAX_BONES);
    
    // Calculate byte offset: slot * (MAX_BONES * sizeof(mat4))
    size_t offsetBytes = static_cast<size_t>(slot) * MAX_BONES * sizeof(glm::mat4);
    
    // Copy matrices into ring buffer
    std::memcpy(static_cast<char*>(m_boneBuffers[frameIndex].map()) + offsetBytes,
                matrices.data(), count * sizeof(glm::mat4));
    
    m_boneBuffers[frameIndex].flush();  // CPU→GPU flush
    
    // Return index to pass to vertex shader via push constant
    return slot * MAX_BONES;
}
```

### Skinned Vertex Format

From `src/renderer/Buffer.h`:

```cpp
struct SkinnedVertex {
    glm::vec3 position;   // location 0 — bind-pose position
    glm::vec3 color;      // location 1
    glm::vec3 normal;     // location 2 — bind-pose normal
    glm::vec2 texCoord;   // location 3
    glm::ivec4 joints;    // location 4 — joint indices (0-3)
    glm::vec4  weights;   // location 5 — joint weights (sum to 1.0)

    static VkVertexInputBindingDescription getBindingDescription();
    static std::array<VkVertexInputAttributeDescription, 6> getAttributeDescriptions();
};

struct InstanceData {
    glm::mat4 model;        // locations 6-9 — entity's world transform
    glm::mat4 normalMatrix; // locations 10-13 — normal matrix
    glm::vec4 tint;         // location 14 — color tint (for selection highlight)
    glm::vec4 params;       // location 15 (shininess, metallic, roughness, emissive)
    glm::vec4 texIndices;   // location 16 (x=diffuse, y=normal)
};
```

### GPU Skinning Shader

From `shaders/skinned.vert`:

```glsl
#version 450

// ── Uniform / SSBO bindings ──────────────────────────────────────────────────
layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix;
} ubo;

// Large bone SSBO: ring-buffer of MAX_CHARS * MAX_BONES mat4s
// boneBaseIndex (from push constant) points to the first bone of this entity
layout(std430, binding = 4) readonly buffer BoneMatrices {
    mat4 bones[];
};

layout(push_constant) uniform PC {
    uint boneBaseIndex;  // Base index into bones[] for this entity
} pc;

// ── Vertex attributes — bind-pose geometry + skin data (static buffer) ───────
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in ivec4 inJoints;   // Joint indices (local to this skeleton)
layout(location = 5) in vec4  inWeights;  // Bone weights (sum to 1.0)

// ── Per-instance attributes ────────────────────────────────────────────────
layout(location = 6)  in mat4 inModel;        // World transform
layout(location = 10) in mat4 inNormalMatrix;
layout(location = 14) in vec4 inTint;
layout(location = 15) in vec4 inParams;       // shininess, metallic, roughness, emissive
layout(location = 16) in vec4 inTexIndices;   // diffuse, normal indices

// ── Outputs to fragment shader ────────────────────────────────────────────
layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out vec3 fragWorldNormal;
layout(location = 4) out vec4 fragLightSpacePos;
layout(location = 5) out float fragShininess;
layout(location = 6) out float fragMetallic;
layout(location = 7) out float fragRoughness;
layout(location = 8) out float fragEmissive;
layout(location = 9)  flat out int fragDiffuseIdx;
layout(location = 10) flat out int fragNormalIdx;

void main() {
    // ── GPU Skinning: blend bone transforms by weights ────────────────────
    // Joint indices are local (0-based); offset by boneBaseIndex
    uint base = pc.boneBaseIndex;
    mat4 skinMat = mat4(0.0);
    skinMat += bones[base + inJoints.x] * inWeights.x;
    skinMat += bones[base + inJoints.y] * inWeights.y;
    skinMat += bones[base + inJoints.z] * inWeights.z;
    skinMat += bones[base + inJoints.w] * inWeights.w;

    // Fallback: if all weights are zero, use identity
    if (dot(inWeights, vec4(1.0)) < 0.0001)
        skinMat = mat4(1.0);

    // ── Transform: bind-pose -> skinned local -> world ──────────────────
    vec4 skinnedPos    = skinMat * vec4(inPosition, 1.0);    // Skinned position
    vec3 skinnedNormal = mat3(skinMat) * inNormal;           // Skinned normal

    vec4 worldPos = inModel * skinnedPos;
    gl_Position   = ubo.proj * ubo.view * worldPos;

    // Normal into world space using the normal matrix
    fragWorldNormal   = normalize(mat3(inNormalMatrix) * skinnedNormal);
    fragWorldPos      = worldPos.xyz;
    fragColor         = inColor * inTint.rgb;
    fragTexCoord      = inTexCoord;
    fragLightSpacePos = ubo.lightSpaceMatrix * worldPos;
    fragShininess     = inParams.x;
    fragMetallic      = inParams.y;
    fragRoughness     = inParams.z;
    fragEmissive      = inParams.w;
    fragDiffuseIdx    = int(inTexIndices.x);
    fragNormalIdx     = int(inTexIndices.y);
}
```

**Key GPU Skinning Points:**
1. **Bind-pose data**: Vertex position/normal are in bind pose (rest pose); never updated on CPU
2. **Ring buffer**: `bones[boneBaseIndex + joint]` looks up the correct skinning matrix for this entity's joint
3. **Weight blending**: `skinMat = bones[j0]*w0 + bones[j1]*w1 + bones[j2]*w2 + bones[j3]*w3`
4. **Two transforms**: Apply skinning matrix to bind-pose vertex, then entity's world transform, then camera matrices

---

## Camera Follow System

### Isometric Camera

From `src/terrain/IsometricCamera.h` and `.cpp`:

```cpp
// IsometricCamera.h
class IsometricCamera {
public:
  IsometricCamera();

  void update(float deltaTime, float windowW, float windowH, double mouseX,
              double mouseY, bool middleMouseDown, float scrollDelta);

  glm::mat4 getViewMatrix() const;
  glm::mat4 getProjectionMatrix(float aspect) const;

  glm::vec3 getPosition() const { return m_position; }
  glm::vec3 getTarget() const { return m_target; }
  float getZoom() const { return m_zoom; }

  void setTarget(const glm::vec3 &target) { m_target = target; }
  void setBounds(const glm::vec3 &min, const glm::vec3 &max) {
    m_boundsMin = min;
    m_boundsMax = max;
  }

  // Set the player character position for camera follow
  void setFollowTarget(const glm::vec3 &pos) { m_followTarget = pos; }

  void setAttached(bool attached) { m_attached = attached; }
  bool isAttached() const { return m_attached; }
  void toggleAttached() { m_attached = !m_attached; }

private:
  bool m_attached = true;                           // true = follow player
  glm::vec3 m_target = glm::vec3(100.0f, 0.0f, 100.0f);    // look-at point
  glm::vec3 m_position = glm::vec3(0.0f, 0.0f, 0.0f);
  glm::vec3 m_followTarget = glm::vec3(100.0f, 0.0f, 100.0f);  // player position

  float m_zoom = 25.0f;  // Camera distance from target

  float m_pitch = 56.0f;  // Degrees from horizontal (LoL-style ~55°)
  float m_yaw = 180.0f;   // Degrees — locked facing "north" (-Z = screen up)

  float m_fov = 45.0f;
  float m_nearPlane = 1.0f;
  float m_farPlane = 500.0f;

  // Follow smoothing
  float m_followSmooth = 8.0f;  // lerp speed toward player

  // Forward look-ahead
  float m_lookAhead = 3.0f;  // units ahead of character

  // Zoom limits
  float m_zoomMin = 15.0f;
  float m_zoomMax = 50.0f;
  float m_zoomSpeed = 3.0f;

  float m_zoomTarget = 25.0f;
  float m_zoomSmooth = 10.0f;

  // Edge panning (detached mode)
  float m_edgeMargin = 20.0f;  // pixels
  float m_panSpeed = 60.0f;    // units/sec

  glm::vec2 m_currentVelocity = glm::vec2(0.0f);

  // Middle-mouse drag
  bool m_dragging = false;
  glm::vec2 m_lastMouse = glm::vec2(0.0f, 0.0f);

  // Map bounds
  glm::vec3 m_boundsMin = glm::vec3(0.0f, 0.0f, 0.0f);
  glm::vec3 m_boundsMax = glm::vec3(200.0f, 0.0f, 200.0f);

  void updatePosition();
  void clampTarget();
};

// IsometricCamera.cpp
IsometricCamera::IsometricCamera() { updatePosition(); }

void IsometricCamera::update(float deltaTime, float windowW, float windowH,
                             double mouseX, double mouseY, bool middleMouseDown,
                             float scrollDelta) {
  float yawRad = glm::radians(m_yaw);
  glm::vec3 forward =
      glm::normalize(glm::vec3(-std::sin(yawRad), 0.0f, -std::cos(yawRad)));
  glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));

  if (m_attached) {
    // ── Attached: smooth follow the player character ──────────────────
    float lerpFactor = std::clamp(m_followSmooth * deltaTime, 0.0f, 1.0f);

    glm::vec3 desiredTarget = m_followTarget;
    desiredTarget.z -= m_lookAhead;  // Look-ahead: show more ahead of character

    m_target += (desiredTarget - m_target) * lerpFactor;
    m_currentVelocity = glm::vec2(0.0f);  // Reset pan velocity
  } else {
    // ── Detached: edge panning ────────────────────────────────────────
    glm::vec2 panDirection(0.0f);

    if (mouseX < m_edgeMargin)
      panDirection.x -= 1.0f;
    if (mouseX > windowW - m_edgeMargin)
      panDirection.x += 1.0f;
    if (mouseY < m_edgeMargin)
      panDirection.y += 1.0f;
    if (mouseY > windowH - m_edgeMargin)
      panDirection.y -= 1.0f;

    if (glm::length(panDirection) > 0.0f)
      panDirection = glm::normalize(panDirection);

    float lerpFactor = std::clamp(deltaTime * 15.0f, 0.0f, 1.0f);
    m_currentVelocity += (panDirection * m_panSpeed - m_currentVelocity) * lerpFactor;

    m_target += (right * m_currentVelocity.x + forward * m_currentVelocity.y) * deltaTime;
  }

  // ── Middle-mouse drag (both modes) ──────────────────────────────────
  glm::vec2 currentMouse(static_cast<float>(mouseX), static_cast<float>(mouseY));

  if (middleMouseDown) {
    if (m_dragging) {
      glm::vec2 delta = currentMouse - m_lastMouse;
      float dragScale = m_zoom * 0.003f;
      m_target -= right * delta.x * dragScale;
      m_target += forward * delta.y * dragScale;
    }
    m_dragging = true;
    m_lastMouse = currentMouse;
  } else {
    m_dragging = false;
  }

  // ── Scroll zoom (smooth easing) ─────────────────────────────────────
  if (scrollDelta != 0.0f) {
    m_zoomTarget -= scrollDelta * m_zoomSpeed;
    m_zoomTarget = std::clamp(m_zoomTarget, m_zoomMin, m_zoomMax);
  }
  float zoomLerp = std::clamp(m_zoomSmooth * deltaTime, 0.0f, 1.0f);
  m_zoom += (m_zoomTarget - m_zoom) * zoomLerp;

  clampTarget();
  updatePosition();
}

glm::mat4 IsometricCamera::getViewMatrix() const {
  return glm::lookAt(m_position, m_target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 IsometricCamera::getProjectionMatrix(float aspect) const {
  glm::mat4 proj = glm::perspective(glm::radians(m_fov), aspect, m_nearPlane, m_farPlane);
  proj[1][1] *= -1.0f;  // Vulkan Y-flip
  return proj;
}

void IsometricCamera::updatePosition() {
  float pitchRad = glm::radians(m_pitch);
  float yawRad = glm::radians(m_yaw);

  // Camera offset: behind and above the target
  glm::vec3 offset;
  offset.x = m_zoom * std::cos(pitchRad) * std::sin(yawRad);
  offset.y = m_zoom * std::sin(pitchRad);
  offset.z = m_zoom * std::cos(pitchRad) * std::cos(yawRad);

  m_position = m_target + offset;
}

void IsometricCamera::clampTarget() {
  m_target.x = std::clamp(m_target.x, m_boundsMin.x, m_boundsMax.x);
  m_target.z = std::clamp(m_target.z, m_boundsMin.z, m_boundsMax.z);
}
```

**Camera Follow Behavior:**
1. **Attached mode**: `m_target` smoothly lerps toward `m_followTarget - lookAhead`
2. **Look-ahead**: Camera shows area ahead of character (z -= 3), so player sees movement destination
3. **Smooth zoom**: Scroll delta targets `m_zoomTarget`, which is lerp'd to actual `m_zoom`
4. **Pitch/Yaw**: Fixed at 56° pitch, 180° yaw → isometric top-down view
5. **Middle-mouse drag**: Pans camera in world space (multiplied by zoom for intuitive feel)

---

## Visual Feedback (Click Indicator)

### Click Indicator Renderer

From `src/renderer/ClickIndicatorRenderer.h` and `Renderer::drawFrame()`:

```cpp
// In Renderer::drawFrame() (line ~395–400):
struct ClickAnim {
    glm::vec3 position{};     // World position of click
    float lifetime = 0.0f;
    float maxLife  = 0.25f;   // Disappear after 250ms
};
std::optional<ClickAnim> m_clickAnim;

// Update click animation
if (m_clickAnim) {
    m_clickAnim->lifetime += dt;
    if (m_clickAnim->lifetime >= m_clickAnim->maxLife)
        m_clickAnim.reset();
}

// In recordCommandBuffer() (line ~570–574):
if (m_clickAnim && m_clickIndicatorRenderer) {
    float t_norm = m_clickAnim->lifetime / m_clickAnim->maxLife;  // 0 to 1
    glm::mat4 vp = m_isoCam.getProjectionMatrix(aspect) * m_isoCam.getViewMatrix();
    // Render expanding circle with fade
    m_clickIndicatorRenderer->render(cmd, vp, m_clickAnim->position, t_norm, 1.5f);
}
```

---

## Selection & Formation Movement

### Selection Logic (in Renderer::drawFrame, lines ~191–296)

```cpp
// LEFT MOUSE BUTTON: Marquee selection
if (m_input->isLeftMouseDown()) {
    if (!m_selection.isDragging) {
        m_selection.isDragging = true;
        m_selection.dragStart = m_input->getMousePos();
    }
    m_selection.dragEnd = m_input->getMousePos();
    
    // Draw marquee box
    glm::vec3 tl = screenToWorld(m_selection.dragStart.x, m_selection.dragStart.y);
    glm::vec3 br = screenToWorld(m_selection.dragEnd.x, m_selection.dragEnd.y);
    // ... draw 4 lines connecting corners ...
    
} else if (m_selection.isDragging) {
    m_selection.isDragging = false;
    glm::vec2 start = m_selection.dragStart;
    glm::vec2 end   = m_selection.dragEnd;
    
    float dist = glm::distance(start, end);
    bool isClick = dist < 5.0f;  // Small movement = click
    
    if (isClick) {
        // Check if clicked on a unit
        bool clickedOnUnit = false;
        for (auto e : view) {
            auto& t = view.get<TransformComponent>(e);
            if (glm::distance(worldToScreen(t.position), end) < 20.0f) {
                clickedOnUnit = true;
                break;
            }
        }
        
        if (clickedOnUnit) {
            // Select units near cursor (Shift = add to selection)
            for (auto e : view) {
                auto& t = view.get<TransformComponent>(e);
                auto& s = view.get<SelectableComponent>(e);
                if (glm::distance(worldToScreen(t.position), end) < 20.0f) {
                    s.isSelected = true;
                } else if (!glfwGetKey(m_window.getHandle(), GLFW_KEY_LEFT_SHIFT)) {
                    s.isSelected = false;
                }
            }
        } else {
            // Click on ground: move selected units to location with formation
            glm::vec3 targetWorld = screenToWorld(end.x, end.y);
            auto unitView = m_scene.getRegistry()
                .view<SelectableComponent, CharacterComponent, UnitComponent>();
            
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
                    
                    // Circular formation offset
                    glm::vec3 offset(0.0f);
                    if (numSelected > 1) {
                        float radius = 1.0f + (numSelected * 0.2f);
                        float angle = (6.2831853f / numSelected) * unitIndex;  // 2π / numSelected
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
        // Marquee: select all units in box
        for (auto e : view) {
            auto& t = view.get<TransformComponent>(e);
            auto& s = view.get<SelectableComponent>(e);
            glm::vec2 screenPos = worldToScreen(t.position);
            if (screenPos.x >= min.x && screenPos.x <= max.x &&
                screenPos.y >= min.y && screenPos.y <= max.y) {
                s.isSelected = true;
            } else if (!glfwGetKey(m_window.getHandle(), GLFW_KEY_LEFT_SHIFT)) {
                s.isSelected = false;
            }
        }
    }
}
```

### Formation Movement Algorithm

**Circle Formation**:
- Count selected units: `numSelected`
- Radius scales by unit count: `radius = 1.0f + (numSelected * 0.2f)`
- Each unit gets an angle: `angle = (2π / numSelected) * unitIndex`
- Offset in world space: `(cos(angle) * radius, 0, sin(angle) * radius)`
- Final destination: `targetWorld + offset`

---

## Minion Spawning & Unit System

### Minion Asset Loading (in buildScene, lines ~714–783)

```cpp
// Load minion model and animations for spawning (X key)
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
    }
    m_minionTexIndex = minionTex;

    if (!skinnedData.bindPoseVertices.empty() && !skinnedData.skinVertices.empty()) {
        // Convert SkinnedModelData to StaticSkinnedMesh
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

        // Load idle animation
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
            }
        } catch (const std::exception& ie) {
            spdlog::warn("Could not load minion idle animation: {}", ie.what());
        }
        if (!idleOk)
            m_minionClips.push_back(AnimationClip{});  // Empty fallback

        // Walk animation from base model
        if (!skinnedData.animations.empty()) {
            m_minionClips.push_back(std::move(skinnedData.animations[0]));
        }

        spdlog::info("Minion model and {} animations loaded for spawning", m_minionClips.size());
    }
} catch (const std::exception& e) {
    spdlog::warn("Could not load minion model: {}", e.what());
}
```

### Minion Spawning (in drawFrame, lines ~149–189)

```cpp
// Press X to spawn minion at mouse position
m_spawnTimer -= dt;
if (glfwGetKey(m_window.getHandle(), GLFW_KEY_X) == GLFW_PRESS && m_spawnTimer <= 0.0f) {
    glm::vec2 mpos = m_input->getMousePos();
    glm::vec3 worldPos = screenToWorld(mpos.x, mpos.y);
    
    auto minion = m_scene.createEntity("MeleeMinion");
    auto& t = m_scene.getRegistry().get<TransformComponent>(minion);
    t.position = worldPos;
    t.scale    = glm::vec3(0.05f);
    t.rotation = glm::vec3(0.0f);

    // Add components
    m_scene.getRegistry().emplace<SelectableComponent>(minion, SelectableComponent{ false, 1.0f });
    m_scene.getRegistry().emplace<UnitComponent>(minion, UnitComponent{ UnitComponent::State::IDLE, worldPos, 5.0f });
    m_scene.getRegistry().emplace<CharacterComponent>(minion, CharacterComponent{ worldPos, 5.0f });
    m_scene.getRegistry().emplace<GPUSkinnedMeshComponent>(minion, GPUSkinnedMeshComponent{ m_minionMeshIndex });

    uint32_t flatNorm = 0;
    m_scene.getRegistry().emplace<MaterialComponent>(minion,
        MaterialComponent{ m_minionTexIndex, flatNorm, 0.0f, 0.0f, 0.5f, 0.2f });
    
    // Setup skeleton and animations
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

    m_spawnTimer = 0.5f;  // Debounce
    spdlog::info("Spawned minion at ({}, {}, {})", worldPos.x, worldPos.y, worldPos.z);
}
```

---

## Summary of Frame Update Order

**Per-Frame Execution** (in `Renderer::drawFrame()`):

1. **Input**: Query mouse clicks, scroll, keys
2. **Camera**: Update isometric camera (zoom, pan, follow)
3. **Right-click command**: Unproject to world, set `targetPosition`, spawn click VFX
4. **Selection**: Marquee drag or single-click selection
5. **Minion spawning**: X key creates new unit at mouse
6. **Movement update loop**:
   - For each unit with `hasTarget = true`:
     - Calculate direction to `targetPosition`
     - Smooth rotation via quaternion slerp (20 rad/s)
     - Smooth acceleration (10 units/s²)
     - Update position: `position += direction * currentSpeed * dt`
     - When distance < 0.1: set `hasTarget = false`
7. **Animation state machine**:
   - For each animated character:
     - If `hasTarget`: switch to walk animation (index 1) with 0.15s crossfade
     - Else: switch to idle animation (index 0)
     - Scale walk animation speed: `timeScale = currentSpeed / moveSpeed` (foot slide fix)
     - Update animation playback: sample keyframes, interpolate, blend
     - Write skinning matrices to GPU ring-buffer SSBO
8. **Click VFX**: Update lifetime, fade out after 0.25s
9. **Recording**: Render frame with GPU-skinned meshes and debug overlays
10. **Present**: Swap buffers

This whole loop runs at target refresh rate (typically 60 FPS on 60Hz, 144 FPS on 144Hz).

---

**End of Document**
