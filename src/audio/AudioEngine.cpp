#include "audio/AudioEngine.h"
#include <spdlog/spdlog.h>

namespace glory {

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { shutdown(); }

void AudioEngine::init() {
    m_initialized = true;
    spdlog::info("Audio engine initialized (stub)");
}

void AudioEngine::shutdown() {
    if (!m_initialized) return;
    m_initialized = false;
    spdlog::info("Audio engine shut down");
}

void AudioEngine::update() {}

void AudioEngine::setListenerPosition(const glm::vec3&, const glm::vec3&, const glm::vec3&) {}

uint32_t AudioEngine::loadSound(const std::string& filepath) {
    spdlog::trace("Audio: load sound '{}'", filepath);
    return 0;
}

void AudioEngine::playSound(uint32_t, const glm::vec3&, float) {}
void AudioEngine::stopSound(uint32_t) {}
void AudioEngine::setMasterVolume(float volume) { m_masterVolume = volume; }

} // namespace glory
