#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <any>
#include <cstdint>

namespace glory {

/// Unique identifier for a loaded script.
using ScriptId = uint32_t;

/// Stub script engine – provides the full API surface for sol2+Lua integration
/// without pulling in any Lua dependency.  Every public method logs its intent
/// via spdlog so integration work can be verified before Lua is wired up.
class ScriptEngine {
public:
    ScriptEngine() = default;
    ~ScriptEngine();

    /// Initialise the (future) Lua VM and register core libraries.
    bool init();

    /// Tear down the VM and release all scripts.
    void shutdown();

    /// Load a Lua script from disk.  Returns a ScriptId handle (0 on failure).
    ScriptId loadScript(const std::string& path);

    /// Scan loaded scripts for on-disk changes and reload any that were modified.
    void reloadModified();

    /// Call a named function inside a loaded script.
    /// @param scriptId  Handle returned by loadScript().
    /// @param functionName  Lua function to invoke.
    /// @param args  Arbitrary arguments forwarded to the function (unused in stub).
    void callFunction(ScriptId scriptId,
                      const std::string& functionName,
                      const std::vector<std::any>& args = {});

    /// True after a successful init() and before shutdown().
    bool isInitialised() const { return m_initialised; }

private:
    struct ScriptEntry {
        std::string                        path;
        std::filesystem::file_time_type    lastWriteTime;
    };

    bool                                          m_initialised{false};
    ScriptId                                      m_nextId{1};
    std::unordered_map<ScriptId, ScriptEntry>     m_scripts;
};

} // namespace glory
