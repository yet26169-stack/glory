#include "audio/AudioEngine.h"

#include <spdlog/spdlog.h>
#include <algorithm>

namespace glory {

AudioEngine::AudioEngine()  = default;
AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init() {
    if (m_initialized) return true;

    // TODO: initialise ma_engine + ma_sound_groups here
    spdlog::info("AudioEngine: miniaudio not yet integrated, running in stub mode");

    m_initialized = true;
    return true;
}

void AudioEngine::shutdown() {
    if (!m_initialized) return;

    spdlog::debug("AudioEngine::shutdown (stub)");
    // TODO: ma_engine_uninit(&m_engine);
    m_initialized = false;
}

void AudioEngine::setListenerPosition(const glm::vec3& pos,
                                      const glm::vec3& forward,
                                      const glm::vec3& up) {
    m_listenerPos     = pos;
    m_listenerForward = forward;
    m_listenerUp      = up;
    // TODO: ma_engine_listener_set_position / direction
    spdlog::debug("AudioEngine::setListenerPosition (stub) [{},{},{}]",
                  pos.x, pos.y, pos.z);
}

void AudioEngine::setMasterVolume(float volume) {
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    spdlog::debug("AudioEngine::setMasterVolume {:.2f} (stub)", m_masterVolume);
}

void AudioEngine::setGroupVolume(SoundGroup group, float volume) {
    auto idx = static_cast<size_t>(group);
    if (idx >= static_cast<size_t>(SoundGroup::Count)) return;
    m_groupVolumes[idx] = std::clamp(volume, 0.0f, 1.0f);
    spdlog::debug("AudioEngine::setGroupVolume group={} vol={:.2f} (stub)",
                  idx, m_groupVolumes[idx]);
}

float AudioEngine::getMasterVolume() const { return m_masterVolume; }

float AudioEngine::getGroupVolume(SoundGroup group) const {
    auto idx = static_cast<size_t>(group);
    if (idx >= static_cast<size_t>(SoundGroup::Count)) return 0.0f;
    return m_groupVolumes[idx];
}

} // namespace glory
