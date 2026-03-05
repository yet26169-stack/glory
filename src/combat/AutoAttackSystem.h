#pragma once

#include "minion/MinionSystem.h"

#include <entt.hpp>

namespace glory {

class AutoAttackSystem {
public:
  void update(entt::registry &registry, MinionSystem &minionSystem, float dt);
};

} // namespace glory
