#pragma once
/// RespawnOverlay: death screen countdown + spectate allies button.

#include <entt.hpp>
#include <imgui.h>

namespace glory {

class RespawnOverlay {
public:
    /// Render the respawn countdown overlay. Returns true if the "spectate" button was clicked.
    bool render(const entt::registry& reg, entt::entity player,
                float screenW, float screenH);
};

} // namespace glory
