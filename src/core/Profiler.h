#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace glory {

class Profiler {
public:
    static Profiler& instance() {
        static Profiler s;
        return s;
    }

    void beginFrame();
    void endFrame();

    void beginScope(const std::string& name);
    void endScope(const std::string& name);

    float getFrameTimeMs() const { return m_frameTimeMs; }
    float getFPS()         const { return m_fps; }

    struct ScopeResult {
        std::string name;
        float       ms = 0.0f;
    };
    const std::vector<ScopeResult>& getResults() const { return m_results; }

private:
    Profiler() = default;

    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    TimePoint m_frameStart;
    float     m_frameTimeMs = 0.0f;
    float     m_fps         = 0.0f;

    std::unordered_map<std::string, TimePoint> m_scopeStarts;
    std::vector<ScopeResult> m_results;
};

// RAII scope timer
class ScopedTimer {
public:
    explicit ScopedTimer(const std::string& name) : m_name(name) {
        Profiler::instance().beginScope(name);
    }
    ~ScopedTimer() {
        Profiler::instance().endScope(m_name);
    }
private:
    std::string m_name;
};

#define GLORY_PROFILE_SCOPE(name) ::glory::ScopedTimer _timer##__LINE__(name)
#define GLORY_PROFILE_FUNCTION()  GLORY_PROFILE_SCOPE(__FUNCTION__)

} // namespace glory
