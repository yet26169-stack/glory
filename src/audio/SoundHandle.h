#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace glory {

using SoundId = uint32_t;
static constexpr SoundId INVALID_SOUND = 0;

class AudioResourceManager;

/// Lightweight RAII wrapper around a playing sound voice.
/// Move-only.  When destroyed the underlying voice slot is released
/// back to AudioResourceManager but the sound is NOT stopped — this
/// enables fire-and-forget usage (play, drop handle, sound plays out).
/// Call stop() explicitly if you need to silence it early.
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
    bool    isValid()   const { return m_sound != nullptr; }
    SoundId getId()     const { return m_id; }

private:
    friend class AudioResourceManager;

    SoundId  m_id    = INVALID_SOUND;
    void*    m_sound = nullptr;  // ma_sound* (heap-allocated, owned by ResourceManager voice pool)
    AudioResourceManager* m_manager = nullptr;
    uint32_t m_voiceSlot = UINT32_MAX;
};

} // namespace glory
