#include "input/InputManager.h"
#include "camera/Camera.h"

#include <algorithm>
#include <cmath>

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

bool InputManager::wasLeftClicked() {
  bool clicked = m_leftClicked;
  m_leftClicked = false;
  return clicked;
}

bool InputManager::wasLeftDragReleased() {
  bool released = m_leftDragReleased;
  m_leftDragReleased = false;
  return released;
}

std::pair<glm::vec2, glm::vec2> InputManager::getLeftDragRect() const {
  glm::vec2 cur = getMousePos();
  glm::vec2 mn(std::min(m_leftDragStart.x, cur.x), std::min(m_leftDragStart.y, cur.y));
  glm::vec2 mx(std::max(m_leftDragStart.x, cur.x), std::max(m_leftDragStart.y, cur.y));
  return {mn, mx};
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

  // Detect drag: left button held and moved past dead zone
  if (m_leftButtonDown && !m_leftDragging) {
    glm::vec2 cur = getMousePos();
    float dist = glm::length(cur - m_leftDragStart);
    if (dist > DRAG_DEAD_ZONE) {
      m_leftDragging = true;
    }
  }

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
  float delta = static_cast<float>(yOffset);
  // Always accumulate for isometric camera zoom
  s_activeInput->m_scrollDelta += delta;
  // Also forward to FPS camera if cursor is captured
  if (s_activeInput->m_cursorCaptured)
    s_activeInput->m_camera.processScroll(delta);
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
  if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
    if (!s_activeInput->m_captureEnabled) {
      // MOBA mode: track left-click position for target selection
      double mx, my;
      glfwGetCursorPos(window, &mx, &my);
      glm::vec2 pos(static_cast<float>(mx), static_cast<float>(my));
      s_activeInput->m_leftClickPos = pos;
      s_activeInput->m_leftButtonDown = true;
      s_activeInput->m_leftDragging = false;
      s_activeInput->m_leftDragStart = pos;
    }
  }
  if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
    if (!s_activeInput->m_captureEnabled) {
      if (s_activeInput->m_leftDragging) {
        s_activeInput->m_leftDragReleased = true;
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);
        s_activeInput->m_leftDragEnd = glm::vec2(static_cast<float>(mx), static_cast<float>(my));
      } else if (s_activeInput->m_leftButtonDown) {
        // Small movement — treat as a normal click
        s_activeInput->m_leftClicked = true;
      }
      s_activeInput->m_leftButtonDown = false;
      s_activeInput->m_leftDragging = false;
    }
  }
}

} // namespace glory
