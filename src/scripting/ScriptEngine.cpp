#include "ScriptEngine.h"
#include <spdlog/spdlog.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace glory {

ScriptEngine::~ScriptEngine() {
    if (m_initialised) {
        shutdown();
    }
}

bool ScriptEngine::init() {
    if (m_initialised) {
        spdlog::warn("ScriptEngine::init() called but already initialised");
        return false;
    }

    // TODO: create sol::state, open standard Lua libraries
    spdlog::info("ScriptEngine::init() — stub: Lua VM would be created here");
    m_initialised = true;
    return true;
}

void ScriptEngine::shutdown() {
    if (!m_initialised) {
        spdlog::warn("ScriptEngine::shutdown() called but not initialised");
        return;
    }

    spdlog::info("ScriptEngine::shutdown() — releasing {} script(s)", m_scripts.size());
    m_scripts.clear();
    m_nextId = 1;
    m_initialised = false;
}

ScriptId ScriptEngine::loadScript(const std::string& path) {
    if (!m_initialised) {
        spdlog::error("ScriptEngine::loadScript() called before init()");
        return 0;
    }

    if (!fs::exists(path)) {
        spdlog::warn("ScriptEngine::loadScript() — file not found: {}", path);
        return 0;
    }

    ScriptId id = m_nextId++;
    ScriptEntry entry;
    entry.path          = path;
    entry.lastWriteTime = fs::last_write_time(path);
    m_scripts[id]       = std::move(entry);

    // TODO: execute sol::state::script_file(path)
    spdlog::info("ScriptEngine::loadScript() — stub: loaded '{}' as id {}", path, id);
    return id;
}

void ScriptEngine::reloadModified() {
    if (!m_initialised) return;

    for (auto& [id, entry] : m_scripts) {
        if (!fs::exists(entry.path)) continue;

        auto currentTime = fs::last_write_time(entry.path);
        if (currentTime != entry.lastWriteTime) {
            entry.lastWriteTime = currentTime;
            // TODO: re-execute sol::state::script_file(entry.path)
            spdlog::info("ScriptEngine::reloadModified() — stub: reloaded '{}' (id {})",
                         entry.path, id);
        }
    }
}

void ScriptEngine::callFunction(ScriptId scriptId,
                                const std::string& functionName,
                                const std::vector<std::any>& args) {
    if (!m_initialised) {
        spdlog::error("ScriptEngine::callFunction() called before init()");
        return;
    }

    auto it = m_scripts.find(scriptId);
    if (it == m_scripts.end()) {
        spdlog::warn("ScriptEngine::callFunction() — unknown script id {}", scriptId);
        return;
    }

    // TODO: look up sol::function and invoke with args
    spdlog::debug("ScriptEngine::callFunction() — stub: would call {}() in '{}' with {} arg(s)",
                  functionName, it->second.path, args.size());
}

} // namespace glory
