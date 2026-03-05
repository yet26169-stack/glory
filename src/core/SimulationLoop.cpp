#include "core/SimulationLoop.h"
#include "input/InputManager.h"
#include "map/MapTypes.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace glory {

void SimulationLoop::tick(SimulationContext& ctx, float deltaTime) {
    float cappedDelta = std::min(deltaTime, MAX_DELTA);
    m_accumulator += cappedDelta;

    int steps = 0;
    while (m_accumulator >= FIXED_DT && steps < MAX_STEPS) {
        if (ctx.input)            ctx.input->update(FIXED_DT);
        if (ctx.scene)            ctx.scene->update(FIXED_DT, ctx.currentFrame);
        if (ctx.projectileSystem) ctx.projectileSystem->update(*ctx.scene, FIXED_DT);

        if (ctx.mobaMode && ctx.gameTime) {
            *ctx.gameTime += FIXED_DT;

            if (!ctx.customMap) {
                if (ctx.minionSystem)
                    ctx.minionSystem->update(ctx.scene->getRegistry(), FIXED_DT,
                                             *ctx.gameTime, ctx.heightFn);
                if (ctx.structureSystem)
                    ctx.structureSystem->update(ctx.scene->getRegistry(), FIXED_DT,
                                                *ctx.gameTime);
                if (ctx.jungleSystem)
                    ctx.jungleSystem->update(ctx.scene->getRegistry(), FIXED_DT,
                                             *ctx.gameTime, ctx.heightFn);
                if (ctx.autoAttackSystem && ctx.minionSystem)
                    ctx.autoAttackSystem->update(ctx.scene->getRegistry(),
                                                 *ctx.minionSystem, FIXED_DT);

                // Process structure death events
                if (ctx.structureSystem) {
                    auto structDeaths = ctx.structureSystem->consumeDeathEvents();
                    for (auto& ev : structDeaths) {
                        if (ev.type == EntityType::Inhibitor && ctx.minionSystem) {
                            ctx.minionSystem->notifyInhibitorDestroyed(ev.team, ev.lane);
                        }
                        if (ev.type == EntityType::Nexus) {
                            spdlog::info("GAME OVER! {} wins!",
                                ev.team == TeamID::Blue ? "Red" : "Blue");
                        }
                    }
                }

                // Consume jungle death events (reserved for future reward logic)
                if (ctx.jungleSystem)
                    ctx.jungleSystem->consumeDeathEvents();
            } else {
                // Custom flat map: minion AI + auto-attack only
                if (ctx.minionSystem)
                    ctx.minionSystem->update(ctx.scene->getRegistry(), FIXED_DT,
                                             *ctx.gameTime, ctx.heightFn);
                if (ctx.autoAttackSystem && ctx.minionSystem)
                    ctx.autoAttackSystem->update(ctx.scene->getRegistry(),
                                                 *ctx.minionSystem, FIXED_DT);
            }

            // Renderer-private post-tick work (bone slots, render component assignment)
            if (ctx.postTickCallback)
                ctx.postTickCallback();
        }

        if (ctx.particles) ctx.particles->update(FIXED_DT);
        m_accumulator -= FIXED_DT;
        ++steps;
    }

    m_alpha = (FIXED_DT > 0.0f) ? (m_accumulator / FIXED_DT) : 0.0f;
}

} // namespace glory
