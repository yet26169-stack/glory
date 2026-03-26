#include "audio/AudioEngine.h"
#include "audio/AudioResourceManager.h"
#include "audio/SoundHandle.h"

#include <algorithm>
#include <filesystem>
#include <miniaudio.h>
#include <spdlog/spdlog.h>
#include <utility>

namespace glory {

// ═══ AudioEngine.cpp ═══

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

// ═══ SoundHandle.cpp ═══

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

// ═══ AudioResourceManager.cpp ═══

AudioResourceManager::AudioResourceManager(AudioEngine& engine)
    : m_engine(engine) {}

AudioResourceManager::~AudioResourceManager() {
    stopAll();
    m_sounds.clear();
    m_nameToId.clear();
}

// ── Resource registry ──────────────────────────────────────────────────────────

SoundId AudioResourceManager::loadSound(const std::string& path, SoundGroup group) {
    std::filesystem::path fsPath(path);
    std::string name = fsPath.stem().string();

    if (auto it = m_nameToId.find(name); it != m_nameToId.end()) {
        return it->second;
    }

    // Resolve relative paths against the audio base directory
    std::string resolvedPath = path;
    if (!m_basePath.empty() && fsPath.is_relative()) {
        resolvedPath = m_basePath + path;
    }

    SoundId id = m_nextId++;
    m_sounds[id] = SoundEntry{id, resolvedPath, name, group};
    m_nameToId[name] = id;

    spdlog::debug("AudioResourceManager: registered '{}' -> id={}", resolvedPath, id);
    return id;
}

void AudioResourceManager::unloadSound(SoundId id) {
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) return;
    m_nameToId.erase(it->second.name);
    m_sounds.erase(it);
}

void AudioResourceManager::loadDirectory(const std::string& dirPath, SoundGroup group) {
    namespace fs = std::filesystem;
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        spdlog::debug("AudioResourceManager::loadDirectory '{}' not found", dirPath);
        return;
    }

    for (const auto& entry : fs::recursive_directory_iterator(dirPath)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac") {
            loadSound(entry.path().string(), group);
        }
    }
}

SoundId AudioResourceManager::findSound(const std::string& name) const {
    if (auto it = m_nameToId.find(name); it != m_nameToId.end())
        return it->second;
    return INVALID_SOUND;
}

// ── Voice pool ─────────────────────────────────────────────────────────────────

uint32_t AudioResourceManager::acquireVoice(uint8_t priority) {
    // 1. Find a free slot
    for (uint32_t i = 0; i < MAX_VOICES; ++i) {
        if (!m_voices[i].active) return i;
    }

    // 2. Find a finished (not playing) slot
    for (uint32_t i = 0; i < MAX_VOICES; ++i) {
        auto& v = m_voices[i];
        if (v.sound && !ma_sound_is_playing(static_cast<ma_sound*>(v.sound))) {
            freeVoiceSlot(i);
            return i;
        }
    }

    // 3. Evict lowest-priority, oldest sound
    uint32_t evictIdx = UINT32_MAX;
    uint8_t  lowestPri = 255;
    float    oldestTime = 1e30f;

    for (uint32_t i = 0; i < MAX_VOICES; ++i) {
        auto& v = m_voices[i];
        if (v.priority < lowestPri || (v.priority == lowestPri && v.startTime < oldestTime)) {
            if (v.priority <= priority) {  // only evict equal-or-lower priority
                evictIdx   = i;
                lowestPri  = v.priority;
                oldestTime = v.startTime;
            }
        }
    }

    if (evictIdx != UINT32_MAX) {
        freeVoiceSlot(evictIdx);
        return evictIdx;
    }

    return UINT32_MAX;  // all voices are higher priority
}

void AudioResourceManager::freeVoiceSlot(uint32_t slot) {
    auto& v = m_voices[slot];
    if (v.sound) {
        ma_sound_uninit(static_cast<ma_sound*>(v.sound));
        delete static_cast<ma_sound*>(v.sound);
        v.sound = nullptr;
    }
    v.active = false;
    v.owned  = false;
}

void AudioResourceManager::releaseVoice(uint32_t slot) {
    if (slot >= MAX_VOICES) return;
    m_voices[slot].owned = false;
    // Don't free yet — let the sound finish playing. tick() will recycle it.
}

void AudioResourceManager::tick() {
    m_clock += 1.0f / 60.0f;  // approximate; good enough for eviction ordering

    for (uint32_t i = 0; i < MAX_VOICES; ++i) {
        auto& v = m_voices[i];
        if (!v.active || v.owned) continue;

        // Not owned by a SoundHandle and finished playing → recycle
        if (v.sound && !ma_sound_is_playing(static_cast<ma_sound*>(v.sound))) {
            freeVoiceSlot(i);
        }
    }
}

SoundHandle AudioResourceManager::initVoice(uint32_t slot, SoundId id, SoundGroup group,
                                              bool spatial, const glm::vec3& pos,
                                              float volume, bool loop) {
    SoundHandle handle;

    auto soundIt = m_sounds.find(id);
    if (soundIt == m_sounds.end()) return handle;

    // Skip sounds that have already failed to load
    if (m_failedSounds.count(id)) return handle;

    auto* engine = static_cast<ma_engine*>(m_engine.getEnginePtr());
    if (!engine) return handle;

    auto* maGroup = static_cast<ma_sound_group*>(m_engine.getGroupPtr(group));

    auto* maSound = new ma_sound;

    ma_uint32 flags = MA_SOUND_FLAG_DECODE;  // decode upfront for low-latency
    if (!spatial) flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

    ma_result res = ma_sound_init_from_file(engine, soundIt->second.path.c_str(),
                                             flags, maGroup, nullptr, maSound);
    if (res != MA_SUCCESS) {
        delete maSound;
        m_failedSounds.insert(id);
        spdlog::warn("AudioResourceManager: failed to init sound '{}' (miniaudio error {})",
                     soundIt->second.path, static_cast<int>(res));
        return handle;
    }

    ma_sound_set_volume(maSound, volume);
    ma_sound_set_looping(maSound, loop ? MA_TRUE : MA_FALSE);

    if (spatial) {
        ma_sound_set_position(maSound, pos.x, pos.y, pos.z);
        ma_sound_set_min_distance(maSound, 1.0f);
        ma_sound_set_max_distance(maSound, 100.0f);
        ma_sound_set_attenuation_model(maSound, ma_attenuation_model_inverse);
    }

    ma_sound_start(maSound);

    auto& v      = m_voices[slot];
    v.sound      = maSound;
    v.active     = true;
    v.owned      = true;
    v.startTime  = m_clock;
    v.priority   = loop ? 2 : 1;  // looping sounds have higher priority

    handle.m_id        = id;
    handle.m_sound     = maSound;
    handle.m_manager   = this;
    handle.m_voiceSlot = slot;

    return handle;
}

// ── Playback API ───────────────────────────────────────────────────────────────

SoundHandle AudioResourceManager::play3D(SoundId id, const glm::vec3& position, float volume) {
    if (!m_engine.isInitialized()) return {};
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) return {};

    uint32_t slot = acquireVoice(1);
    if (slot == UINT32_MAX) return {};

    return initVoice(slot, id, it->second.group, true, position, volume, false);
}

SoundHandle AudioResourceManager::play2D(SoundId id, float volume) {
    if (!m_engine.isInitialized()) return {};
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) return {};

    uint32_t slot = acquireVoice(1);
    if (slot == UINT32_MAX) return {};

    return initVoice(slot, id, it->second.group, false, {}, volume, false);
}

SoundHandle AudioResourceManager::playMusic(SoundId id, float volume) {
    if (!m_engine.isInitialized()) return {};
    auto it = m_sounds.find(id);
    if (it == m_sounds.end()) return {};

    uint32_t slot = acquireVoice(3);  // music = highest priority
    if (slot == UINT32_MAX) return {};

    return initVoice(slot, id, SoundGroup::Music, false, {}, volume, true);
}

void AudioResourceManager::stopAll() {
    for (uint32_t i = 0; i < MAX_VOICES; ++i) {
        if (m_voices[i].active) {
            if (m_voices[i].sound) {
                ma_sound_stop(static_cast<ma_sound*>(m_voices[i].sound));
            }
            freeVoiceSlot(i);
        }
    }
}

} // namespace glory
