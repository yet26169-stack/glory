#pragma once

#include <glm/glm.hpp>
#include <string>
#include <cstdint>

namespace glory {

enum class SoundGroup : uint8_t {
    SFX   = 0,
    Music = 1,
    Voice = 2,
    UI    = 3,
    Count = 4
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool init();
    void shutdown();

    void setListenerPosition(const glm::vec3& pos,
                             const glm::vec3& forward,
                             const glm::vec3& up);

    void  setMasterVolume(float volume);
    void  setGroupVolume(SoundGroup group, float volume);
    float getMasterVolume() const;
    float getGroupVolume(SoundGroup group) const;

    bool isInitialized() const { return m_initialized; }

private:
    bool  m_initialized  = false;
    float m_masterVolume = 1.0f;
    float m_groupVolumes[static_cast<size_t>(SoundGroup::Count)] = {1.0f, 0.7f, 1.0f, 1.0f};

    glm::vec3 m_listenerPos     {0.0f};
    glm::vec3 m_listenerForward {0.0f, 0.0f, -1.0f};
    glm::vec3 m_listenerUp      {0.0f, 1.0f, 0.0f};

    // miniaudio handles will go here once integrated
    // ma_engine      m_engine;
    // ma_sound_group m_groups[4];
};

} // namespace glory
