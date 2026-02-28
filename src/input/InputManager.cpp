#include "input/InputManager.h"
#include "camera/Camera.h"

namespace glory {

// Static instance for GLFW callbacks (single-window engine)
static InputManager *s_activeInput = nullptr;

InputManager::InputManager(GLFWwindow *window, Camera &camera)
    : m_window(window), m_camera(camera) {
  s_activeInput = this;
  glfwSetKeyCallback(m_window, keyCallback);
  glfwSetCursorPosCallback(m_window, mouseCallback);
  glfwSetScrollCallback(m_window, scrollCallback);
  glfwSetMouseButtonCallback(m_window, mouseButtonCallback);
}

bool InputManager::wasF1Pressed() {
  bool pressed = m_f1Pressed;
  m_f1Pressed = false;
  return pressed;
}

bool InputManager::wasF2Pressed() {
  bool pressed = m_f2Pressed;
  m_f2Pressed = false;
  return pressed;
}

bool InputManager::wasF3Pressed() {
  bool pressed = m_f3Pressed;
  m_f3Pressed = false;
  return pressed;
}

bool InputManager::wasF4Pressed() {
  bool pressed = m_f4Pressed;
  m_f4Pressed = false;
  return pressed;
}

bool InputManager::wasYPressed() {
  bool pressed = m_yPressed;
  m_yPressed = false;
  return pressed;
}

bool InputManager::wasRightClicked() {
  bool clicked = m_rightClicked;
  m_rightClicked = false;
  return clicked;
}

bool InputManager::wasQPressed() {
  bool p = m_qPressed;
  m_qPressed = false;
  return p;
}

bool InputManager::wasWAbilityPressed() {
  bool p = m_wAbilityPressed;
  m_wAbilityPressed = false;
  return p;
}

bool InputManager::wasEPressed() {
  bool p = m_ePressed;
  m_ePressed = false;
  return p;
}

bool InputManager::wasRPressed() {
  bool p = m_rPressed;
  m_rPressed = false;
  return p;
}

bool InputManager::wasAbilityPressed(AbilitySlot slot) {
  switch (slot) {
  case AbilitySlot::Q:
    return wasQPressed();
  case AbilitySlot::W:
    return wasWAbilityPressed();
  case AbilitySlot::E:
    return wasEPressed();
  case AbilitySlot::R:
    return wasRPressed();
  default:
    return false;
  }
}

void InputManager::keyCallback(GLFWwindow * /*window*/, int key,
                               int /*scancode*/, int action, int /*mods*/) {
  if (!s_activeInput)
    return;
  if (key == GLFW_KEY_F1 && action == GLFW_PRESS)
    s_activeInput->m_f1Pressed = true;
  if (key == GLFW_KEY_F2 && action == GLFW_PRESS)
    s_activeInput->m_f2Pressed = true;
  if (key == GLFW_KEY_F3 && action == GLFW_PRESS)
    s_activeInput->m_f3Pressed = true;
  if (key == GLFW_KEY_F4 && action == GLFW_PRESS)
    s_activeInput->m_f4Pressed = true;
  if (key == GLFW_KEY_Y && action == GLFW_PRESS)
    s_activeInput->m_yPressed = true;

  // Ability keys (only when cursor is NOT captured, i.e. MOBA mode)
  if (!s_activeInput->m_cursorCaptured) {
    if (key == GLFW_KEY_Q && action == GLFW_PRESS)
      s_activeInput->m_qPressed = true;
    if (key == GLFW_KEY_W && action == GLFW_PRESS)
      s_activeInput->m_wAbilityPressed = true;
    if (key == GLFW_KEY_E && action == GLFW_PRESS)
      s_activeInput->m_ePressed = true;
    if (key == GLFW_KEY_R && action == GLFW_PRESS)
      s_activeInput->m_rPressed = true;
  }
}

void InputManager::update(float deltaTime) {
  bool forward = glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS;
  bool backward = glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS;
  bool left = glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS;
  bool right = glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS;
  bool up = glfwGetKey(m_window, GLFW_KEY_SPACE) == GLFW_PRESS;
  bool down = glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;

  m_camera.processKeyboard(deltaTime, forward, backward, left, right, up, down);

  // Escape releases cursor
  if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS && m_cursorCaptured) {
    m_cursorCaptured = false;
    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    m_firstMouse = true;
  }
}

void InputManager::mouseCallback(GLFWwindow * /*window*/, double xPos,
                                 double yPos) {
  if (!s_activeInput || !s_activeInput->m_cursorCaptured)
    return;
  auto *self = s_activeInput;

  if (self->m_firstMouse) {
    self->m_lastX = xPos;
    self->m_lastY = yPos;
    self->m_firstMouse = false;
  }

  auto xOffset = static_cast<float>(xPos - self->m_lastX);
  auto yOffset = static_cast<float>(self->m_lastY - yPos); // inverted Y
  self->m_lastX = xPos;
  self->m_lastY = yPos;

  self->m_camera.processMouse(xOffset, yOffset);
}

void InputManager::scrollCallback(GLFWwindow * /*window*/, double /*xOffset*/,
                                  double yOffset) {
  if (!s_activeInput)
    return;
  s_activeInput->m_camera.processScroll(static_cast<float>(yOffset));
}

void InputManager::mouseButtonCallback(GLFWwindow *window, int button,
                                       int action, int /*mods*/) {
  if (!s_activeInput)
    return;
  if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
    if (s_activeInput->m_captureEnabled) {
      s_activeInput->m_cursorCaptured = true;
      glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      s_activeInput->m_firstMouse = true;
    } else {
      // MOBA mode: track right-click position for click-to-move
      double mx, my;
      glfwGetCursorPos(window, &mx, &my);
      s_activeInput->m_rightClickPos =
          glm::vec2(static_cast<float>(mx), static_cast<float>(my));
      s_activeInput->m_rightClicked = true;
    }
  }
}

} // namespace glory
