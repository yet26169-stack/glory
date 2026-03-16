#pragma once
/// Floating combat text — damage numbers, heals, crits.
///
/// When damage/healing occurs, spawn a floating number at the target's
/// world position.  Each frame, project to screen coords, float upward,
/// fade out over ~1 s.  Rendered via ImGui overlay.

#include <glm/glm.hpp>
#include <imgui.h>

#include <array>
#include <cstdint>
#include <string>

namespace glory {

class FloatingText {
public:
    enum class DamageKind : uint8_t {
        Physical, // white
        Magic,    // purple
        True,     // white, bold
        Heal,     // green
        Crit      // yellow, larger
    };

    /// Spawn a new floating number.
    void spawn(const glm::vec3& worldPos, float amount, DamageKind kind);

    /// Update and render all active texts.
    ///   vp         – combined view-projection matrix
    ///   screenW/H  – window dimensions in pixels
    ///   dt         – frame delta time
    void update(const glm::mat4& vp, float screenW, float screenH, float dt);

    static constexpr uint32_t MAX_TEXTS = 64;

private:
    struct Entry {
        bool      active    = false;
        float     age       = 0.0f;
        float     lifetime  = 1.0f;
        glm::vec3 worldPos  {0.f};
        float     amount    = 0.0f;
        DamageKind kind     = DamageKind::Physical;
        float     riseSpeed = 60.0f;  // pixels/s upward
        float     xJitter   = 0.0f;   // slight horizontal scatter
    };

    std::array<Entry, MAX_TEXTS> m_pool{};
    uint32_t m_next = 0; // round-robin index

    static ImU32 colorForKind(DamageKind k);
    static float sizeForKind(DamageKind k);
};

} // namespace glory
