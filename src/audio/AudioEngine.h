#pragma once

#include <glm/glm.hpp>
#include <string>

namespace glory {

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    void init();
    void shutdown();
    void update();

    void setListenerPosition(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up);

    uint32_t loadSound(const std::string& filepath);
    void playSound(uint32_t soundId, const glm::vec3& position = glm::vec3(0.0f), float volume = 1.0f);
    void stopSound(uint32_t soundId);
    void setMasterVolume(float volume);

    bool isInitialized() const { return m_initialized; }

private:
    bool  m_initialized = false;
    float m_masterVolume = 1.0f;
};

struct AudioSourceComponent {
    uint32_t soundId = 0;
    float    volume  = 1.0f;
    bool     loop    = false;
    bool     playing = false;
};

} // namespace glory
