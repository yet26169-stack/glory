#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace glory {

class IsometricCamera {
public:
  IsometricCamera();

  void update(float deltaTime, float windowW, float windowH, double mouseX,
              double mouseY, bool middleMouseDown, float scrollDelta);

  glm::mat4 getViewMatrix() const;
  glm::mat4 getProjectionMatrix(float aspect) const;

  // Returns ray origin and direction for a screen pixel (mouse coords)
  void screenToWorldRay(float mouseX, float mouseY, float windowW, float windowH,
                        glm::vec3 &outOrigin, glm::vec3 &outDir) const;

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

  // Attached (follows player) vs detached (free pan) mode
  void setAttached(bool attached) { m_attached = attached; }
  bool isAttached() const { return m_attached; }
  void toggleAttached() { m_attached = !m_attached; }

private:
  bool m_attached = true; // true = follow player, false = free pan
  glm::vec3 m_target = glm::vec3(100.0f, 0.0f, 100.0f); // look-at point
  glm::vec3 m_position = glm::vec3(0.0f, 0.0f, 0.0f);
  glm::vec3 m_followTarget = glm::vec3(100.0f, 0.0f, 100.0f); // player pos
  float m_zoom = 25.0f; // camera distance from target

  float m_pitch = 56.0f;  // degrees from horizontal (LoL-style)
  float m_yaw = 180.0f;   // degrees — locked facing "north" (-Z = screen up)

  float m_fov = 45.0f;    // degrees — wider perspective for depth
  float m_nearPlane = 1.0f;
  float m_farPlane = 500.0f;

  // Camera follow smoothing
  float m_followSmooth = 8.0f; // lerp speed

  // Forward look-ahead offset (sees more ahead of the character)
  float m_lookAhead = 3.0f;

  // Zoom limits
  float m_zoomMin = 15.0f;
  float m_zoomMax = 50.0f;
  float m_zoomSpeed = 3.0f;

  // Smooth zoom easing
  float m_zoomTarget = 25.0f; // desired zoom (set by scroll)
  float m_zoomSmooth = 10.0f; // lerp speed toward target

  // Edge panning (detached mode)
  float m_edgeMargin = 20.0f; // pixels
  float m_panSpeed = 60.0f;   // units/sec
  glm::vec2 m_currentVelocity = glm::vec2(0.0f);

  // Middle-mouse drag
  bool m_dragging = false;
  glm::vec2 m_lastMouse = glm::vec2(0.0f, 0.0f);

  // Map bounds clamping
  glm::vec3 m_boundsMin = glm::vec3(0.0f, 0.0f, 0.0f);
  glm::vec3 m_boundsMax = glm::vec3(200.0f, 0.0f, 200.0f);

  void updatePosition();
  void clampTarget();
};

} // namespace glory
