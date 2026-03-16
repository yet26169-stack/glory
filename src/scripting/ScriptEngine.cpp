#include "ScriptEngine.h"

#include <sol/sol.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace glory {

ScriptEngine::ScriptEngine() = default;

ScriptEngine::~ScriptEngine() {
    if (m_initialised) shutdown();
}

bool ScriptEngine::init() {
    if (m_initialised) {
        spdlog::warn("[ScriptEngine] init() called but already initialised");
        return false;
    }

    m_lua = std::make_unique<sol::state>();

    // Sandboxed: open only safe libraries (no io, os, debug)
    m_lua->open_libraries(sol::lib::base, sol::lib::math,
                          sol::lib::string, sol::lib::table,
                          sol::lib::coroutine);

    // Override dangerous globals
    (*m_lua)["dofile"]    = sol::lua_nil;
    (*m_lua)["loadfile"]  = sol::lua_nil;
    (*m_lua)["require"]   = sol::lua_nil;

    m_initialised = true;
    spdlog::info("[ScriptEngine] Lua 5.4 VM initialised (sandboxed)");
    return true;
}

void ScriptEngine::shutdown() {
    if (!m_initialised) return;

    spdlog::info("[ScriptEngine] shutdown — releasing {} script(s)", m_scripts.size());
    m_scripts.clear();
    m_lua.reset();
    m_nextId = 1;
    m_initialised = false;
}

ScriptId ScriptEngine::loadScript(const std::string& path) {
    if (!m_initialised) {
        spdlog::error("[ScriptEngine] loadScript() called before init()");
        return 0;
    }

    if (!fs::exists(path)) {
        spdlog::warn("[ScriptEngine] loadScript — file not found: {}", path);
        return 0;
    }

    try {
        m_lua->safe_script_file(path);
    } catch (const sol::error& e) {
        spdlog::error("[ScriptEngine] loadScript '{}' failed: {}", path, e.what());
        return 0;
    }

    ScriptId id = m_nextId++;
    m_scripts[id] = ScriptEntry{path, fs::last_write_time(path)};
    spdlog::info("[ScriptEngine] loaded '{}' as id {}", path, id);
    return id;
}

void ScriptEngine::reloadModified() {
    if (!m_initialised) return;

    for (auto& [id, entry] : m_scripts) {
        if (!fs::exists(entry.path)) continue;

        auto currentTime = fs::last_write_time(entry.path);
        if (currentTime != entry.lastWriteTime) {
            entry.lastWriteTime = currentTime;
            try {
                m_lua->safe_script_file(entry.path);
                spdlog::info("[ScriptEngine] hot-reloaded '{}' (id {})", entry.path, id);
            } catch (const sol::error& e) {
                spdlog::error("[ScriptEngine] hot-reload '{}' failed: {}", entry.path, e.what());
            }
        }
    }
}

void ScriptEngine::callFunction(const std::string& functionName) {
    if (!m_initialised) return;

    sol::protected_function fn = (*m_lua)[functionName];
    if (!fn.valid()) return;

    auto result = fn();
    if (!result.valid()) {
        sol::error err = result;
        spdlog::error("[ScriptEngine] {}() error: {}", functionName, err.what());
    }
}

void ScriptEngine::callFunction(const std::string& functionName,
                                uint32_t casterEntity, uint32_t targetEntity,
                                float px, float py, float pz) {
    if (!m_initialised) return;

    sol::protected_function fn = (*m_lua)[functionName];
    if (!fn.valid()) return;

    auto result = fn(casterEntity, targetEntity, px, py, pz);
    if (!result.valid()) {
        sol::error err = result;
        spdlog::error("[ScriptEngine] {}() error: {}", functionName, err.what());
    }
}

sol::state& ScriptEngine::lua() {
    return *m_lua;
}

} // namespace glory
