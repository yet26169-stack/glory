#pragma once
/// Kill feed — top-right corner event log.
///
/// Shows "[Killer] killed [Victim]" messages.  Max 5 visible, each fades
/// after 5 seconds.

#include <imgui.h>
#include <string>
#include <deque>

namespace glory {

class KillFeed {
public:
    struct Event {
        std::string killerName;
        std::string victimName;
        uint8_t     killerTeam = 0;
        uint8_t     victimTeam = 1;
        float       age        = 0.0f;
    };

    /// Push a new kill event.
    void push(const std::string& killer, uint8_t killerTeam,
              const std::string& victim, uint8_t victimTeam);

    /// Update timers and render.
    void render(float screenW, float dt);

    static constexpr int    MAX_VISIBLE  = 5;
    static constexpr float  FADE_TIME    = 5.0f;
    static constexpr float  FADE_START   = 3.5f; // start fading at this age

private:
    std::deque<Event> m_events;
};

} // namespace glory
