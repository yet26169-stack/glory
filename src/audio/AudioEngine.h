#pragma once

#include <glm/glm.hpp>
#include <string>
#include <cstdint>
#include <memory>

namespace glory {

enum class SoundGroup : uint8_t {
    SFX   = 0,
    Music = 1,
    Voice = 2,
    UI    = 3,
    Count = 4
};

struct AudioEngineImpl;

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

    /// Opaque pointer to the underlying ma_engine (for internal use by SoundHandle/ResourceManager).
    void* getEnginePtr() const;

    /// Opaque pointer to a ma_sound_group for the given SoundGroup.
    void* getGroupPtr(SoundGroup group) const;

private:
    bool  m_initialized  = false;
    float m_masterVolume = 1.0f;
    float m_groupVolumes[static_cast<size_t>(SoundGroup::Count)] = {1.0f, 0.7f, 1.0f, 1.0f};

    std::unique_ptr<AudioEngineImpl> m_impl;
};

} // namespace glory
