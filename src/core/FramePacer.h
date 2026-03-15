#pragma once

#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace glory {

// Hybrid spin-sleep frame pacer with platform-precise timing.
//
// Uses a coarse OS sleep to yield the CPU when > kSpinThreshold remains,
// then a tight spin-loop for the final 2ms to hit the exact target time.
//
// Windows: timeBeginPeriod(1) sets 1ms scheduler granularity;
//          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION gives ~100ns waits (Win10 1803+).
// Linux:   nanosleep() + optional SCHED_FIFO elevation.
// macOS:   nanosleep() coarse sleep + spin.
class FramePacer {
public:
    explicit FramePacer(int targetFps = 60);
    ~FramePacer();

    // 0 = uncapped
    void setTargetFps(int fps);
    int  getTargetFps() const { return m_targetFps; }

    // Call at the very start of the main loop iteration.
    void beginFrame();

    // Call after rendering. Sleeps/spins until the target frame time is reached.
    void endFrame();

    // Measured wall-clock time of the most recently completed frame (ms).
    double getLastFrameMs() const { return m_lastFrameMs; }

    FramePacer(const FramePacer&)            = delete;
    FramePacer& operator=(const FramePacer&) = delete;

private:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Nanos     = std::chrono::nanoseconds;

    int       m_targetFps   = 60;
    Nanos     m_targetFrame {};      // nanoseconds per frame; 0 = uncapped
    TimePoint m_frameStart  {};
    double    m_lastFrameMs = 0.0;

    void precisionWait(TimePoint target);

#ifdef _WIN32
    HANDLE m_hTimer                 = nullptr;
    bool   m_highResTimerAvailable  = false;
#endif
};

} // namespace glory
