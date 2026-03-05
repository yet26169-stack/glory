#pragma once
#include <entt.hpp>

namespace glory {

/// Writes SimPosition (simulation authority) → TransformComponent (render view).
/// Called once per render frame, after all simulation ticks, before rendering.
/// alpha = interpolation factor within the current fixed tick (0..1) — reserved
/// for future sub-frame position blending.
class SimToRenderSyncSystem {
public:
    static void sync(entt::registry& reg, float alpha);
};

} // namespace glory
