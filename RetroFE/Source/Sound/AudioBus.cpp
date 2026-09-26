#include "AudioBus.h"
#include "MusicPlayer.h"

#include "../Utility/Log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace {
constexpr int kFadeChunkFrames = 256;
constexpr int kMaxChannels = 8;
}

AudioBus::Source::~Source() { shutdown(); }

void AudioBus::Source::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    // Track destruction waits for the mixer to finish reading the stream.
    if (track) {
        MIX_DestroyTrack(track);
        track = nullptr;
    }
    if (stream) {
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }
}

AudioBus& AudioBus::instance() {
    static AudioBus bus;
    return bus;
}

AudioBus::~AudioBus() { shutdown(); }

bool AudioBus::initialize(int sampleRate, int channels) {
    if (mixer_)
        return true;
    if (sampleRate <= 0 || channels <= 0 || channels > kMaxChannels)
        return SDL_SetError("Invalid audio rate or channel count");

    if (!MIX_Init())
        return false;
    mixerInitialized_ = true;

    const SDL_AudioSpec requested{ SDL_AUDIO_F32, channels, sampleRate };
    mixer_ = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &requested);
    if (!mixer_) {
        shutdown();
        return false;
    }

    // Keep the producer format stable across hardware device changes.
    sourceRate_ = sampleRate;
    sourceChannels_ = channels;
    LOG_INFO("AudioBus", "Initialized stream mixer: " +
             std::to_string(sourceRate_) + " Hz, " +
             std::to_string(sourceChannels_) + " producer channels");
    return true;
}

void AudioBus::shutdown() {
    MusicPlayer* player = nullptr;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        player = std::exchange(musicPlayer_, nullptr);
    }
    if (player)
        player->releaseAudio();

    std::vector<std::weak_ptr<Source>> oldSources;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        oldSources.swap(sources_);
    }
    for (auto& entry : oldSources) {
        if (auto source = entry.lock())
            source->shutdown();
    }

    if (mixer_) {
        MIX_DestroyMixer(mixer_);
        mixer_ = nullptr;
        ++generation_;
    }
    if (mixerInitialized_) {
        MIX_Quit();
        mixerInitialized_ = false;
    }
}

void AudioBus::configureFromMixer() {
    if (!initialize())
        LOG_ERROR("AudioBus", std::string("Audio initialization failed: ") + SDL_GetError());
}

void AudioBus::setMusicPlayer(MusicPlayer* player) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    musicPlayer_ = player;
}

std::shared_ptr<AudioBus::Handle> AudioBus::createSource(
    const char* name, size_t queueLimitKB, OverflowPolicy overflowPolicy) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!mixer_) {
        SDL_SetError("Audio mixer is not initialized");
        return nullptr;
    }

    const auto fail = [name]() -> std::shared_ptr<Handle> {
        LOG_ERROR("AudioBus", std::string("Unable to create ") +
                  (name ? name : "audio source") + ": " + SDL_GetError());
        return nullptr;
    };

    const size_t bytesPerFrame = sizeof(float) * static_cast<size_t>(sourceChannels_);
    if (queueLimitKB > static_cast<size_t>(std::numeric_limits<int>::max()) / 1024) {
        SDL_SetError("Audio source queue limit is too large");
        return fail();
    }

    auto source = std::make_shared<Source>();
    source->queueLimitBytes = std::max(queueLimitKB * 1024, bytesPerFrame);
    source->overflowPolicy = overflowPolicy;

    const SDL_AudioSpec spec{ SDL_AUDIO_F32, sourceChannels_, sourceRate_ };
    source->stream = SDL_CreateAudioStream(&spec, &spec);
    if (!source->stream)
        return fail();

    source->track = MIX_CreateTrack(mixer_);
    if (!source->track || !MIX_SetTrackAudioStream(source->track, source->stream))
        return fail();

    const SDL_PropertiesID options = SDL_CreateProperties();
    if (!options)
        return fail();
    const bool started =
        SDL_SetBooleanProperty(options, MIX_PROP_PLAY_HALT_WHEN_EXHAUSTED_BOOLEAN, false) &&
        MIX_PlayTrack(source->track, options);
    SDL_DestroyProperties(options);
    if (!started)
        return fail();

    sources_.erase(std::remove_if(sources_.begin(), sources_.end(),
                                  [](const auto& weak) { return weak.expired(); }),
                   sources_.end());
    sources_.push_back(source);
    return std::shared_ptr<Handle>(new Handle(std::move(source)));
}

void AudioBus::setGain(const std::shared_ptr<Handle>& handle, float gain) noexcept {
    if (!handle || !handle->sp)
        return;
    auto& source = *handle->sp;
    std::lock_guard<std::mutex> lock(source.mutex);
    if (source.track)
        MIX_SetTrackGain(source.track, std::clamp(gain, 0.0f, 1.0f));
}

void AudioBus::clear(const std::shared_ptr<Handle>& handle) noexcept {
    if (!handle || !handle->sp)
        return;
    auto& source = *handle->sp;
    std::lock_guard<std::mutex> lock(source.mutex);
    if (source.stream)
        SDL_ClearAudioStream(source.stream);
}

void AudioBus::triggerFadeIn(const std::shared_ptr<Handle>& handle,
                             int durationFrames) noexcept {
    if (!handle || !handle->sp)
        return;
    auto& source = *handle->sp;
    std::lock_guard<std::mutex> lock(source.mutex);
    source.fadeTotalFrames = std::max(0, durationFrames);
    source.fadeConsumedFrames = 0;
}

void AudioBus::push(const std::shared_ptr<Handle>& handle, const void* data, int bytes) {
    if (handle && handle->sp)
        pushImpl(*handle->sp, data, bytes);
}

void AudioBus::pushImpl(Source& source, const void* data, int bytes) {
    if (!data || bytes <= 0)
        return;

    std::lock_guard<std::mutex> lock(source.mutex);
    if (!source.stream)
        return;

    const int bytesPerFrame = sourceChannels_ * static_cast<int>(sizeof(float));
    bytes -= bytes % bytesPerFrame;
    if (bytes <= 0)
        return;

    const int queued = SDL_GetAudioStreamQueued(source.stream);
    if (queued < 0)
        return;
    if (static_cast<size_t>(queued) + static_cast<size_t>(bytes) > source.queueLimitBytes) {
        if (source.overflowPolicy == OverflowPolicy::RejectNewest ||
            static_cast<size_t>(bytes) > source.queueLimitBytes)
            return;
        if (!SDL_ClearAudioStream(source.stream))
            return;
        source.fadeTotalFrames = kDefaultFadeFrames;
        source.fadeConsumedFrames = 0;
    }

    const auto* input = static_cast<const uint8_t*>(data);
    const int frames = bytes / bytesPerFrame;
    int writtenFrames = 0;
    std::array<float, kFadeChunkFrames * kMaxChannels> scratch;

    while (writtenFrames < frames && source.fadeConsumedFrames < source.fadeTotalFrames) {
        const int count = std::min({ frames - writtenFrames,
                                     source.fadeTotalFrames - source.fadeConsumedFrames,
                                     kFadeChunkFrames });
        for (int frame = 0; frame < count; ++frame) {
            const int fadeFrame = source.fadeConsumedFrames + frame;
            const float fade = source.fadeTotalFrames <= 1
                ? 1.0f
                : static_cast<float>(fadeFrame) / static_cast<float>(source.fadeTotalFrames - 1);
            for (int channel = 0; channel < sourceChannels_; ++channel) {
                float sample;
                std::memcpy(&sample, input + (writtenFrames + frame) * bytesPerFrame +
                                     channel * sizeof(float), sizeof(float));
                scratch[frame * sourceChannels_ + channel] = sample * fade;
            }
        }
        if (!SDL_PutAudioStreamData(source.stream, scratch.data(), count * bytesPerFrame))
            return;
        source.fadeConsumedFrames += count;
        writtenFrames += count;
    }

    if (writtenFrames < frames)
        SDL_PutAudioStreamData(source.stream, input + writtenFrames * bytesPerFrame,
                               (frames - writtenFrames) * bytesPerFrame);
}
