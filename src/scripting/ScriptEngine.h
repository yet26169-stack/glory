#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <cstdint>
#include <memory>

// Forward-declare sol types to keep enet/Vulkan headers out
namespace sol { class state; }

namespace glory {

using ScriptId = uint32_t;

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();

    /// Initialise the Lua VM and register core libraries (no io/os/debug).
    bool init();

    /// Tear down the VM and release all scripts.
    void shutdown();

    /// Load a Lua script from disk. Returns a ScriptId handle (0 on failure).
    ScriptId loadScript(const std::string& path);

    /// Scan loaded scripts for on-disk changes and reload any that were modified.
    void reloadModified();

    /// Call a named global Lua function with no arguments.
    void callFunction(const std::string& functionName);

    /// Call a named global Lua function with entity IDs and position.
    void callFunction(const std::string& functionName,
                      uint32_t casterEntity, uint32_t targetEntity,
                      float px, float py, float pz);

    /// Direct access to the sol::state for binding registration.
    sol::state& lua();

    bool isInitialised() const { return m_initialised; }

private:
    struct ScriptEntry {
        std::string                     path;
        std::filesystem::file_time_type lastWriteTime;
    };

    bool                                      m_initialised{false};
    ScriptId                                  m_nextId{1};
    std::unordered_map<ScriptId, ScriptEntry> m_scripts;
    std::unique_ptr<sol::state>               m_lua;
};

} // namespace glory
