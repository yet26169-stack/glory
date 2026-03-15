#include "core/FramePacer.h"
#include "core/Profiler.h"

#include <thread>

#ifdef _WIN32
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

#if defined(__linux__)
#include <sched.h>
#include <cerrno>
#include <time.h>
#elif defined(__APPLE__)
#include <time.h>
#endif

namespace glory {

FramePacer::FramePacer(int targetFps) {
#ifdef _WIN32
    // Request 1ms OS scheduler granularity for the lifetime of this object.
    timeBeginPeriod(1);

    // High-resolution waitable timer: available on Windows 10 Build 1803+.
    // Gives ~100ns wake precision without needing elevated privileges.
    m_hTimer = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    m_highResTimerAvailable = (m_hTimer != nullptr);

    if (!m_highResTimerAvailable) {
        // Fallback: regular waitable timer still benefits from timeBeginPeriod(1).
        m_hTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
#endif

#ifdef __linux__
    // Attempt SCHED_FIFO elevation for better sleep precision.
    // Requires CAP_SYS_NICE or root — silently ignored if unavailable.
    struct sched_param param{};
    param.sched_priority = 1; // lowest FIFO priority
    sched_setscheduler(0, SCHED_FIFO, &param);
#endif

    setTargetFps(targetFps);
    m_frameStart = Clock::now();
}

FramePacer::~FramePacer() {
#ifdef _WIN32
    timeEndPeriod(1);
    if (m_hTimer) {
        CloseHandle(m_hTimer);
        m_hTimer = nullptr;
    }
#endif
}

void FramePacer::setTargetFps(int fps) {
    m_targetFps = fps;
    if (fps <= 0) {
        m_targetFrame = Nanos{0};
    } else {
        m_targetFrame = Nanos{1'000'000'000LL / fps};
    }
}

void FramePacer::beginFrame() {
    m_frameStart = Clock::now();
}

void FramePacer::endFrame() {
    auto frameEnd = Clock::now();
    m_lastFrameMs = std::chrono::duration<double, std::milli>(frameEnd - m_frameStart).count();

    if (m_targetFrame.count() == 0) return; // uncapped

    precisionWait(m_frameStart + m_targetFrame);
}

void FramePacer::precisionWait(TimePoint target) {
    auto now = Clock::now();
    if (now >= target) return; // frame already over budget — skip wait

    auto remaining = std::chrono::duration_cast<Nanos>(target - now);

    // Spin-threshold: stop sleeping and start spinning this many ns before target.
    static constexpr Nanos kSpinThreshold{2'000'000}; // 2ms

    // ── Coarse OS sleep ─────────────────────────────────────────────────────
    if (remaining > kSpinThreshold) {
        auto sleepDur = remaining - kSpinThreshold;
        GLORY_ZONE_N("OSSleep");

#ifdef _WIN32
        if (m_hTimer) {
            // SetWaitableTimer uses 100ns units; negative = relative wait.
            LARGE_INTEGER dueTime;
            dueTime.QuadPart = -(sleepDur.count() / 100LL);
            SetWaitableTimer(m_hTimer, &dueTime, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(m_hTimer, INFINITE);
        } else {
            std::this_thread::sleep_for(sleepDur);
        }
#elif defined(__linux__) || defined(__APPLE__)
        struct timespec ts{};
        ts.tv_sec  = sleepDur.count() / 1'000'000'000LL;
        ts.tv_nsec = sleepDur.count() % 1'000'000'000LL;
        // Retry on signal interruption.
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
#else
        std::this_thread::sleep_for(sleepDur);
#endif
    }

    // ── Tight spin for the final 2ms ────────────────────────────────────────
    {
        GLORY_ZONE_N("SpinWait");
        while (Clock::now() < target) {
            // busy-wait — intentional; yields nothing to maintain precision
        }
    }
}

} // namespace glory
