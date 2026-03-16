#include "audio/AudioEngine.h"

#include <miniaudio.h>
#include <spdlog/spdlog.h>
#include <algorithm>

namespace glory {

// ── Pimpl holding miniaudio objects (too large / platform-specific for header) ─
struct AudioEngineImpl {
    ma_engine      engine{};
    ma_sound_group groups[static_cast<size_t>(SoundGroup::Count)]{};
};

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init() {
    if (m_initialized) return true;

    m_impl = std::make_unique<AudioEngineImpl>();

    ma_engine_config cfg = ma_engine_config_init();
    cfg.listenerCount = 1;
    cfg.channels      = 2;  // stereo

    ma_result res = ma_engine_init(&cfg, &m_impl->engine);
    if (res != MA_SUCCESS) {
        spdlog::error("AudioEngine: ma_engine_init failed ({})", static_cast<int>(res));
        m_impl.reset();
        return false;
    }

    // Create sound groups (one per SoundGroup enum value)
    for (size_t i = 0; i < static_cast<size_t>(SoundGroup::Count); ++i) {
        res = ma_sound_group_init(&m_impl->engine, 0, nullptr, &m_impl->groups[i]);
        if (res != MA_SUCCESS) {
            spdlog::warn("AudioEngine: failed to create sound group {} ({})",
                         i, static_cast<int>(res));
        }
        ma_sound_group_set_volume(&m_impl->groups[i], m_groupVolumes[i]);
    }

    m_initialized = true;
    spdlog::info("AudioEngine: initialised (miniaudio {})", ma_version_string());
    return true;
}

void AudioEngine::shutdown() {
    if (!m_initialized) return;

    for (size_t i = 0; i < static_cast<size_t>(SoundGroup::Count); ++i) {
        ma_sound_group_uninit(&m_impl->groups[i]);
    }
    ma_engine_uninit(&m_impl->engine);
    m_impl.reset();

    m_initialized = false;
    spdlog::info("AudioEngine: shut down");
}

void AudioEngine::setListenerPosition(const glm::vec3& pos,
                                      const glm::vec3& forward,
                                      const glm::vec3& up) {
    if (!m_initialized) return;
    ma_engine_listener_set_position(&m_impl->engine, 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(&m_impl->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&m_impl->engine, 0, up.x, up.y, up.z);
}

void AudioEngine::setMasterVolume(float volume) {
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    if (m_initialized) {
        ma_engine_set_volume(&m_impl->engine, m_masterVolume);
    }
}

void AudioEngine::setGroupVolume(SoundGroup group, float volume) {
    auto idx = static_cast<size_t>(group);
    if (idx >= static_cast<size_t>(SoundGroup::Count)) return;
    m_groupVolumes[idx] = std::clamp(volume, 0.0f, 1.0f);
    if (m_initialized) {
        ma_sound_group_set_volume(&m_impl->groups[idx], m_groupVolumes[idx]);
    }
}

float AudioEngine::getMasterVolume() const { return m_masterVolume; }

float AudioEngine::getGroupVolume(SoundGroup group) const {
    auto idx = static_cast<size_t>(group);
    if (idx >= static_cast<size_t>(SoundGroup::Count)) return 0.0f;
    return m_groupVolumes[idx];
}

void* AudioEngine::getEnginePtr() const {
    return m_impl ? &m_impl->engine : nullptr;
}

void* AudioEngine::getGroupPtr(SoundGroup group) const {
    auto idx = static_cast<size_t>(group);
    if (!m_impl || idx >= static_cast<size_t>(SoundGroup::Count)) return nullptr;
    return &m_impl->groups[idx];
}

} // namespace glory
