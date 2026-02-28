#include "core/Profiler.h"

namespace glory {

void Profiler::beginFrame() {
    m_frameStart = Clock::now();
    m_results.clear();
}

void Profiler::endFrame() {
    auto now = Clock::now();
    m_frameTimeMs = std::chrono::duration<float, std::milli>(now - m_frameStart).count();
    m_fps = (m_frameTimeMs > 0.0f) ? 1000.0f / m_frameTimeMs : 0.0f;
}

void Profiler::beginScope(const std::string& name) {
    m_scopeStarts[name] = Clock::now();
}

void Profiler::endScope(const std::string& name) {
    auto it = m_scopeStarts.find(name);
    if (it != m_scopeStarts.end()) {
        float ms = std::chrono::duration<float, std::milli>(Clock::now() - it->second).count();
        m_results.push_back({name, ms});
        m_scopeStarts.erase(it);
    }
}

} // namespace glory
