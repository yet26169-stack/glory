#pragma once

#include "core/ThreadPool.h"

#include <entt.hpp>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>

namespace glory {

// ── ISystem: base interface for all ECS systems ─────────────────────────────
// Systems declare their dependencies so the scheduler can build a DAG and
// identify parallelizable levels.
class ISystem {
public:
    virtual ~ISystem() = default;

    // Execute one tick of this system.
    virtual void execute(entt::registry& registry, float dt) = 0;

    // Return type_index of systems that must complete before this one runs.
    virtual std::vector<std::type_index> dependsOn() const { return {}; }

    // Human-readable name for profiling/logging.
    virtual std::string_view name() const = 0;
};

// ── SystemScheduler ─────────────────────────────────────────────────────────
// Manages a collection of ISystem instances, builds a dependency DAG,
// topologically sorts them into parallelizable "levels," and executes
// each level in parallel using a ThreadPool.
class SystemScheduler {
public:
    SystemScheduler() = default;

    // Register a system. Ownership is transferred.
    template<typename T, typename... Args>
    T* add(Args&&... args) {
        auto sys = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = sys.get();
        m_systems.push_back(std::move(sys));
        m_dirty = true;
        return ptr;
    }

    // Build the dependency DAG and compute execution levels.
    // Must be called after all systems are registered and before tick().
    void build();

    // Execute all systems in dependency order.
    // Systems at the same level run in parallel via the ThreadPool.
    void tick(entt::registry& registry, float dt, ThreadPool& pool);

    // Execute all systems in dependency order on the calling thread.
    void tickSequential(entt::registry& registry, float dt);

    // Number of registered systems.
    size_t systemCount() const { return m_systems.size(); }

    // Number of execution levels (available after build()).
    size_t levelCount() const { return m_levels.size(); }

private:
    std::vector<std::unique_ptr<ISystem>> m_systems;

    // Each level is a set of system indices that can run in parallel.
    std::vector<std::vector<uint32_t>> m_levels;
    bool m_dirty = true;
};

} // namespace glory
