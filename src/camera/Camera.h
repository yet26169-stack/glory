#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace glory {

class Camera {
public:
    Camera(float fovDegrees = 45.0f, float nearPlane = 0.1f, float farPlane = 100.0f);

    glm::mat4 getViewMatrix() const;
    glm::mat4 getProjectionMatrix(float aspectRatio) const;

    void processKeyboard(float deltaTime, bool forward, bool backward,
                         bool left, bool right, bool up, bool down);
    void processMouse(float xOffset, float yOffset);
    void processScroll(float yOffset);

    glm::vec3 getPosition() const { return m_position; }
    void setPosition(const glm::vec3& pos) { m_position = pos; }

private:
    void updateVectors();

    glm::vec3 m_position = {0.0f, 0.0f, 3.0f};
    glm::vec3 m_front    = {0.0f, 0.0f, -1.0f};
    glm::vec3 m_up       = {0.0f, 1.0f, 0.0f};
    glm::vec3 m_right    = {1.0f, 0.0f, 0.0f};
    glm::vec3 m_worldUp  = {0.0f, 1.0f, 0.0f};

    float m_yaw   = -90.0f;
    float m_pitch = 0.0f;

    float m_movementSpeed    = 2.5f;
    float m_mouseSensitivity = 0.1f;
    float m_fov;
    float m_nearPlane;
    float m_farPlane;
};

} // namespace glory
