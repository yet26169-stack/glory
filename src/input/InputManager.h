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

  bool wasF4Pressed();
  bool wasYPressed();
  // Ability hotkeys – consume-once pattern, same as wasF4Pressed()
  bool wasQPressed();
  bool wasWPressed();
  bool wasEPressed();
  bool wasRPressed();
  bool wasRightClicked();
  glm::vec2 getLastClickPos() const { return m_rightClickPos; }
  bool wasLeftClicked();
  glm::vec2 getLastLeftClickPos() const { return m_leftClickPos; }
  bool isLeftMouseDown() const { return glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS; }
  bool isRightMouseDown() const { return glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS; }

  // ── Combat keys (A/S/D) and debug UI (Tab) ─────────────────────────────
  bool wasTabPressed();
  bool wasAPressed();
  bool wasDPressed();
  bool wasSPressed();
  bool isSHeld() const { return m_sHeld; }
  bool wasSReleased();
  glm::vec2 getMousePos() const {
    double x, y;
    glfwGetCursorPos(m_window, &x, &y);
    return glm::vec2(static_cast<float>(x), static_cast<float>(y));
  }

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
  bool m_captureEnabled = true;
  bool m_f4Pressed = false;
  bool m_yPressed  = false;
  bool m_qPressed  = false;
  bool m_wPressed  = false;
  bool m_ePressed  = false;
  bool m_rPressed  = false;
  bool m_rightClicked = false;
  glm::vec2 m_rightClickPos{0.0f};
  bool m_leftClicked = false;
  glm::vec2 m_leftClickPos{0.0f};

  // Combat / debug keys
  bool m_tabPressed  = false;
  bool m_aPressed    = false;
  bool m_dPressed    = false;
  bool m_sPressed    = false;
  bool m_sHeld       = false;   // true while S is physically held down
  bool m_sReleased   = false;   // consume-once: set on S key-up

  float m_scrollDelta = 0.0f;

  static void keyCallback(GLFWwindow *window, int key, int scancode, int action, int mods);
  static void mouseCallback(GLFWwindow *window, double xPos, double yPos);
  static void scrollCallback(GLFWwindow *window, double xOffset, double yOffset);
  static void mouseButtonCallback(GLFWwindow *window, int button, int action, int mods);
};

} // namespace glory
