#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace glory {

using SoundId = uint32_t;
static constexpr SoundId INVALID_SOUND = 0;

class SoundHandle {
public:
    SoundHandle() = default;
    ~SoundHandle();

    SoundHandle(SoundHandle&& other) noexcept;
    SoundHandle& operator=(SoundHandle&& other) noexcept;
    SoundHandle(const SoundHandle&)            = delete;
    SoundHandle& operator=(const SoundHandle&) = delete;

    void play();
    void stop();
    void pause();
    void resume();

    void setPosition(const glm::vec3& pos);
    void setVolume(float volume);
    void setPitch(float pitch);
    void setLooping(bool loop);

    bool    isPlaying() const;
    bool    isValid()   const { return m_id != INVALID_SOUND; }
    SoundId getId()     const { return m_id; }

private:
    friend class AudioResourceManager;
    SoundId   m_id       = INVALID_SOUND;
    bool      m_playing  = false;
    bool      m_looping  = false;
    float     m_volume   = 1.0f;
    float     m_pitch    = 1.0f;
    glm::vec3 m_position {0.0f};
};

} // namespace glory
