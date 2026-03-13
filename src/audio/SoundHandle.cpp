#include "audio/SoundHandle.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <utility>

namespace glory {

SoundHandle::~SoundHandle() {
    if (m_id != INVALID_SOUND) {
        spdlog::debug("SoundHandle::~SoundHandle id={} (stub)", m_id);
        // TODO: ma_sound_uninit
    }
}

SoundHandle::SoundHandle(SoundHandle&& other) noexcept
    : m_id(other.m_id)
    , m_playing(other.m_playing)
    , m_looping(other.m_looping)
    , m_volume(other.m_volume)
    , m_pitch(other.m_pitch)
    , m_position(other.m_position) {
    other.m_id      = INVALID_SOUND;
    other.m_playing = false;
}

SoundHandle& SoundHandle::operator=(SoundHandle&& other) noexcept {
    if (this != &other) {
        if (m_id != INVALID_SOUND) {
            spdlog::debug("SoundHandle move-assign releasing id={} (stub)", m_id);
        }
        m_id       = std::exchange(other.m_id, INVALID_SOUND);
        m_playing  = std::exchange(other.m_playing, false);
        m_looping  = other.m_looping;
        m_volume   = other.m_volume;
        m_pitch    = other.m_pitch;
        m_position = other.m_position;
    }
    return *this;
}

void SoundHandle::play() {
    if (m_id == INVALID_SOUND) return;
    m_playing = true;
    spdlog::debug("SoundHandle::play id={} (stub)", m_id);
    // TODO: ma_sound_start
}

void SoundHandle::stop() {
    if (m_id == INVALID_SOUND) return;
    m_playing = false;
    spdlog::debug("SoundHandle::stop id={} (stub)", m_id);
    // TODO: ma_sound_stop
}

void SoundHandle::pause() {
    if (m_id == INVALID_SOUND) return;
    m_playing = false;
    spdlog::debug("SoundHandle::pause id={} (stub)", m_id);
    // TODO: ma_sound_stop (miniaudio uses stop for pause)
}

void SoundHandle::resume() {
    if (m_id == INVALID_SOUND) return;
    m_playing = true;
    spdlog::debug("SoundHandle::resume id={} (stub)", m_id);
    // TODO: ma_sound_start
}

void SoundHandle::setPosition(const glm::vec3& pos) {
    m_position = pos;
    spdlog::debug("SoundHandle::setPosition id={} (stub)", m_id);
    // TODO: ma_sound_set_position
}

void SoundHandle::setVolume(float volume) {
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    spdlog::debug("SoundHandle::setVolume id={} vol={:.2f} (stub)", m_id, m_volume);
    // TODO: ma_sound_set_volume
}

void SoundHandle::setPitch(float pitch) {
    m_pitch = std::max(pitch, 0.01f);
    spdlog::debug("SoundHandle::setPitch id={} pitch={:.2f} (stub)", m_id, m_pitch);
    // TODO: ma_sound_set_pitch
}

void SoundHandle::setLooping(bool loop) {
    m_looping = loop;
    spdlog::debug("SoundHandle::setLooping id={} loop={} (stub)", m_id, loop);
    // TODO: ma_sound_set_looping
}

bool SoundHandle::isPlaying() const { return m_playing; }

} // namespace glory
