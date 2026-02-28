#pragma once

#include "ability/AbilityTypes.h"

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace glory {

class Camera;

class InputManager {
public:
  InputManager(GLFWwindow *window, Camera &camera);
  ~InputManager() = default;

  void update(float deltaTime);

  bool isCursorCaptured() const { return m_cursorCaptured; }
  void setCaptureEnabled(bool enabled) { m_captureEnabled = enabled; }
  bool wasF1Pressed();
  bool wasF2Pressed();
  bool wasF3Pressed();
  bool wasF4Pressed();
  bool wasYPressed();
  bool wasRightClicked();
  glm::vec2 getLastClickPos() const { return m_rightClickPos; }
  glm::vec2 getMousePos() const {
    double x, y;
    glfwGetCursorPos(m_window, &x, &y);
    return glm::vec2(static_cast<float>(x), static_cast<float>(y));
  }

  // Ability keys (MOBA mode)
  bool wasAbilityPressed(AbilitySlot slot);
  bool wasQPressed();
  bool wasEPressed();
  bool wasRPressed();
  bool wasWAbilityPressed(); // Distinct from camera W

private:
  GLFWwindow *m_window;
  Camera &m_camera;

  double m_lastX = 0.0;
  double m_lastY = 0.0;
  bool m_firstMouse = true;
  bool m_cursorCaptured = false;
  bool m_captureEnabled = true;
  bool m_f1Pressed = false;
  bool m_f2Pressed = false;
  bool m_f3Pressed = false;
  bool m_f4Pressed = false;
  bool m_yPressed = false;
  bool m_rightClicked = false;
  glm::vec2 m_rightClickPos{0.0f};

  // Ability key states (MOBA mode)
  bool m_qPressed = false;
  bool m_wAbilityPressed = false;
  bool m_ePressed = false;
  bool m_rPressed = false;

  static void keyCallback(GLFWwindow *window, int key, int scancode, int action,
                          int mods);
  static void mouseCallback(GLFWwindow *window, double xPos, double yPos);
  static void scrollCallback(GLFWwindow *window, double xOffset,
                             double yOffset);
  static void mouseButtonCallback(GLFWwindow *window, int button, int action,
                                  int mods);
};

} // namespace glory
