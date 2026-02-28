#include "camera/Camera.h"

#include <algorithm>

namespace glory {

Camera::Camera(float fovDegrees, float nearPlane, float farPlane)
    : m_fov(fovDegrees), m_nearPlane(nearPlane), m_farPlane(farPlane)
{
    updateVectors();
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(m_position, m_position + m_front, m_up);
}

glm::mat4 Camera::getProjectionMatrix(float aspectRatio) const {
    auto proj = glm::perspective(glm::radians(m_fov), aspectRatio, m_nearPlane, m_farPlane);
    proj[1][1] *= -1.0f; // Vulkan Y-axis is inverted vs OpenGL
    return proj;
}

void Camera::processKeyboard(float deltaTime, bool forward, bool backward,
                              bool left, bool right, bool up, bool down)
{
    float velocity = m_movementSpeed * deltaTime;
    if (forward)  m_position += m_front   * velocity;
    if (backward) m_position -= m_front   * velocity;
    if (left)     m_position -= m_right   * velocity;
    if (right)    m_position += m_right   * velocity;
    if (up)       m_position += m_worldUp * velocity;
    if (down)     m_position -= m_worldUp * velocity;
}

void Camera::processMouse(float xOffset, float yOffset) {
    m_yaw   += xOffset * m_mouseSensitivity;
    m_pitch += yOffset * m_mouseSensitivity;
    m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);
    updateVectors();
}

void Camera::processScroll(float yOffset) {
    m_fov -= yOffset;
    m_fov = std::clamp(m_fov, 1.0f, 120.0f);
}

void Camera::updateVectors() {
    glm::vec3 front;
    front.x = cos(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    front.y = sin(glm::radians(m_pitch));
    front.z = sin(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    m_front = glm::normalize(front);
    m_right = glm::normalize(glm::cross(m_front, m_worldUp));
    m_up    = glm::normalize(glm::cross(m_right, m_front));
}

} // namespace glory
