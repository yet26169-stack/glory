#pragma once

#include "hud/Minimap.h"

namespace glory {

class HUD {
public:
    HUD() = default;

    void update(MinimapUpdateContext& ctx) { m_minimap.update(ctx); }

    bool isMinimapHovered() const { return m_minimap.isInteracting(); }

private:
    Minimap m_minimap;
};

} // namespace glory
