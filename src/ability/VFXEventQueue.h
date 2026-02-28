#pragma once

#include <glm/glm.hpp>
#include <mutex>
#include <string>
#include <vector>

namespace glory {

enum class VFXEventType { CAST, PROJECTILE_SPAWN, IMPACT };

struct VFXEvent {
  VFXEventType type;
  glm::vec3 position;
  glm::vec3 direction;
  std::string vfxId;
  std::string sfxId;
};

// Thread-safe MPSC queue for transferring game logic VFX requests to the
// Renderer
class VFXEventQueue {
public:
  static VFXEventQueue &get() {
    static VFXEventQueue instance;
    return instance;
  }

  void enqueue(VFXEventType type, const glm::vec3 &position,
               const glm::vec3 &direction, const std::string &vfxId,
               const std::string &sfxId = "") {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back({type, position, direction, vfxId, sfxId});
  }

  std::vector<VFXEvent> consumeAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<VFXEvent> consumed = std::move(m_events);
    m_events.clear();
    return consumed;
  }

private:
  VFXEventQueue() = default;
  std::vector<VFXEvent> m_events;
  std::mutex m_mutex;
};

} // namespace glory
