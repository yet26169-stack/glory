#include "audio/SoundHandle.h"
#include "audio/AudioResourceManager.h"

#include <miniaudio.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <utility>

namespace glory {

SoundHandle::~SoundHandle() {
    // Release the voice slot back to the pool.
    // We do NOT stop the sound — fire-and-forget plays to completion.
    if (m_manager && m_voiceSlot != UINT32_MAX) {
        m_manager->releaseVoice(m_voiceSlot);
    }
    m_sound = nullptr;
    m_manager = nullptr;
}

SoundHandle::SoundHandle(SoundHandle&& other) noexcept
    : m_id(other.m_id)
    , m_sound(other.m_sound)
    , m_manager(other.m_manager)
    , m_voiceSlot(other.m_voiceSlot) {
    other.m_id        = INVALID_SOUND;
    other.m_sound     = nullptr;
    other.m_manager   = nullptr;
    other.m_voiceSlot = UINT32_MAX;
}

SoundHandle& SoundHandle::operator=(SoundHandle&& other) noexcept {
    if (this != &other) {
        // Release our current voice
        if (m_manager && m_voiceSlot != UINT32_MAX) {
            m_manager->releaseVoice(m_voiceSlot);
        }
        m_id        = std::exchange(other.m_id, INVALID_SOUND);
        m_sound     = std::exchange(other.m_sound, nullptr);
        m_manager   = std::exchange(other.m_manager, nullptr);
        m_voiceSlot = std::exchange(other.m_voiceSlot, UINT32_MAX);
    }
    return *this;
}

void SoundHandle::play() {
    if (!m_sound) return;
    ma_sound_start(static_cast<ma_sound*>(m_sound));
}

void SoundHandle::stop() {
    if (!m_sound) return;
    ma_sound_stop(static_cast<ma_sound*>(m_sound));
}

void SoundHandle::pause() {
    if (!m_sound) return;
    ma_sound_stop(static_cast<ma_sound*>(m_sound));
}

void SoundHandle::resume() {
    if (!m_sound) return;
    ma_sound_start(static_cast<ma_sound*>(m_sound));
}

void SoundHandle::setPosition(const glm::vec3& pos) {
    if (!m_sound) return;
    ma_sound_set_position(static_cast<ma_sound*>(m_sound), pos.x, pos.y, pos.z);
}

void SoundHandle::setVolume(float volume) {
    if (!m_sound) return;
    ma_sound_set_volume(static_cast<ma_sound*>(m_sound), std::clamp(volume, 0.0f, 2.0f));
}

void SoundHandle::setPitch(float pitch) {
    if (!m_sound) return;
    ma_sound_set_pitch(static_cast<ma_sound*>(m_sound), std::max(pitch, 0.01f));
}

void SoundHandle::setLooping(bool loop) {
    if (!m_sound) return;
    ma_sound_set_looping(static_cast<ma_sound*>(m_sound), loop ? MA_TRUE : MA_FALSE);
}

bool SoundHandle::isPlaying() const {
    if (!m_sound) return false;
    return ma_sound_is_playing(static_cast<ma_sound*>(m_sound)) == MA_TRUE;
}

} // namespace glory
