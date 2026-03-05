#include "core/SimToRenderSyncSystem.h"
#include "scene/Components.h"

namespace glory {

void SimToRenderSyncSystem::sync(entt::registry& reg, float alpha) {
    (void)alpha; // reserved for future sub-frame interpolation
    auto view = reg.view<SimPosition, TransformComponent>();
    for (auto e : view) {
        auto& sim = view.get<SimPosition>(e);
        auto& xf  = view.get<TransformComponent>(e);
#ifdef GLORY_DETERMINISTIC
        xf.position = glm::vec3(sim.value.x.toFloat(),
                                sim.value.y.toFloat(),
                                sim.value.z.toFloat());
#else
        xf.position = sim.value;
#endif
    }
}

} // namespace glory
