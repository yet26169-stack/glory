#include "terrain/IsometricCamera.h"

#include <algorithm>
#include <cmath>

namespace glory {

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
    desiredTarget.z -= m_lookAhead; // look-ahead: show more in front

    m_target += (desiredTarget - m_target) * lerpFactor;
    m_currentVelocity = glm::vec2(0.0f); // reset pan velocity
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
    m_currentVelocity +=
        (panDirection * m_panSpeed - m_currentVelocity) * lerpFactor;

    m_target +=
        (right * m_currentVelocity.x + forward * m_currentVelocity.y) *
        deltaTime;
  }

  // ── Middle-mouse drag (both modes) ──────────────────────────────────
  glm::vec2 currentMouse(static_cast<float>(mouseX),
                         static_cast<float>(mouseY));

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

  // ── Scroll zoom ─────────────────────────────────────────────────────────
  if (scrollDelta != 0.0f) {
    m_zoom -= scrollDelta * m_zoomSpeed;
    m_zoom = std::clamp(m_zoom, m_zoomMin, m_zoomMax);
  }

  clampTarget();
  updatePosition();
}

glm::mat4 IsometricCamera::getViewMatrix() const {
  return glm::lookAt(m_position, m_target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 IsometricCamera::getProjectionMatrix(float aspect) const {
  glm::mat4 proj =
      glm::perspective(glm::radians(m_fov), aspect, m_nearPlane, m_farPlane);
  proj[1][1] *= -1.0f; // Vulkan Y-flip
  return proj;
}

void IsometricCamera::screenToWorldRay(float mouseX, float mouseY,
                                       float windowW, float windowH,
                                       glm::vec3 &outOrigin,
                                       glm::vec3 &outDir) const {
  float aspect = windowW / windowH;
  glm::mat4 view = getViewMatrix();
  glm::mat4 proj = getProjectionMatrix(aspect);

  // Undo Vulkan Y-flip for unprojection
  glm::mat4 projNoFlip = proj;
  projNoFlip[1][1] *= -1.0f;

  glm::mat4 invVP = glm::inverse(projNoFlip * view);

  // Convert screen coords to NDC [-1, 1]
  // Screen Y goes top-down, NDC Y goes bottom-up → negate
  float ndcX = (2.0f * mouseX / windowW) - 1.0f;
  float ndcY = -((2.0f * mouseY / windowH) - 1.0f);

  // Near and far points in NDC (Vulkan NDC z: 0 to 1)
  glm::vec4 nearNDC(ndcX, ndcY, 0.0f, 1.0f);
  glm::vec4 farNDC(ndcX, ndcY, 1.0f, 1.0f);

  glm::vec4 nearWorld = invVP * nearNDC;
  glm::vec4 farWorld = invVP * farNDC;
  nearWorld /= nearWorld.w;
  farWorld /= farWorld.w;

  outOrigin = glm::vec3(nearWorld);
  outDir = glm::normalize(glm::vec3(farWorld) - glm::vec3(nearWorld));
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

} // namespace glory
