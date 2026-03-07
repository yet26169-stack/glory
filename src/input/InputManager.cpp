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

bool InputManager::wasF4Pressed() { bool p = m_f4Pressed; m_f4Pressed = false; return p; }
bool InputManager::wasYPressed()  { bool p = m_yPressed;  m_yPressed  = false; return p; }
bool InputManager::wasQPressed()  { bool p = m_qPressed;  m_qPressed  = false; return p; }
bool InputManager::wasWPressed()  { bool p = m_wPressed;  m_wPressed  = false; return p; }
bool InputManager::wasEPressed()  { bool p = m_ePressed;  m_ePressed  = false; return p; }
bool InputManager::wasRPressed()  { bool p = m_rPressed;  m_rPressed  = false; return p; }
bool InputManager::wasRightClicked() { bool p = m_rightClicked; m_rightClicked = false; return p; }
bool InputManager::wasLeftClicked()  { bool p = m_leftClicked;  m_leftClicked  = false; return p; }

void InputManager::keyCallback(GLFWwindow*, int key, int, int action, int) {
  if (!s_activeInput) return;
  if (key == GLFW_KEY_F4 && action == GLFW_PRESS) s_activeInput->m_f4Pressed = true;
  if (key == GLFW_KEY_Y  && action == GLFW_PRESS) s_activeInput->m_yPressed  = true;
  if (key == GLFW_KEY_Q  && action == GLFW_PRESS) s_activeInput->m_qPressed  = true;
  if (key == GLFW_KEY_W  && action == GLFW_PRESS) s_activeInput->m_wPressed  = true;
  if (key == GLFW_KEY_E  && action == GLFW_PRESS) s_activeInput->m_ePressed  = true;
  if (key == GLFW_KEY_R  && action == GLFW_PRESS) s_activeInput->m_rPressed  = true;
  if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && s_activeInput->m_cursorCaptured) {
    s_activeInput->m_cursorCaptured = false;
    glfwSetInputMode(s_activeInput->m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    s_activeInput->m_firstMouse = true;
  }
}

void InputManager::update(float deltaTime) {
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

void InputManager::mouseCallback(GLFWwindow*, double xPos, double yPos) {
  if (!s_activeInput || !s_activeInput->m_cursorCaptured) return;
  auto* self = s_activeInput;
  if (self->m_firstMouse) { self->m_lastX = xPos; self->m_lastY = yPos; self->m_firstMouse = false; }
  float dx = static_cast<float>(xPos - self->m_lastX);
  float dy = static_cast<float>(self->m_lastY - yPos);
  self->m_lastX = xPos; self->m_lastY = yPos;
  self->m_camera.processMouse(dx, dy);
}

void InputManager::scrollCallback(GLFWwindow*, double, double yOffset) {
  if (!s_activeInput) return;
  float d = static_cast<float>(yOffset);
  s_activeInput->m_scrollDelta += d;
  if (s_activeInput->m_cursorCaptured) s_activeInput->m_camera.processScroll(d);
}

void InputManager::mouseButtonCallback(GLFWwindow* window, int button, int action, int) {
  if (!s_activeInput) return;
  if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
    if (s_activeInput->m_captureEnabled) {
      s_activeInput->m_cursorCaptured = true;
      glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      s_activeInput->m_firstMouse = true;
    } else {
      double mx, my; glfwGetCursorPos(window, &mx, &my);
      s_activeInput->m_rightClickPos = glm::vec2(static_cast<float>(mx), static_cast<float>(my));
      s_activeInput->m_rightClicked = true;
    }
  }
  if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS && !s_activeInput->m_captureEnabled) {
    double mx, my; glfwGetCursorPos(window, &mx, &my);
    s_activeInput->m_leftClickPos = glm::vec2(static_cast<float>(mx), static_cast<float>(my));
    s_activeInput->m_leftClicked = true;
  }
}

} // namespace glory
