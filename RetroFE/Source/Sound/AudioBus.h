#pragma once

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class MusicPlayer;

// Owns the shared mixer and the streaming tracks used by video backends.
// Producers supply native-endian, interleaved F32 at dev_rate()/dev_channels().
class AudioBus {
    struct Source;

public:
    static constexpr int kDefaultFadeFrames = 256;

    static AudioBus& instance();
    // Main-thread lifecycle. Stop external producers before shutdown.
    bool initialize(int sampleRate = 48000, int channels = 2);
    void shutdown();
    void configureFromMixer();

    MIX_Mixer* mixer() const { return mixer_; }
    uint64_t generation() const { return generation_; }
    void setMusicPlayer(MusicPlayer* player);

    class Handle {
        friend class AudioBus;
        std::shared_ptr<Source> sp;
        explicit Handle(std::shared_ptr<Source> source) : sp(std::move(source)) {}
    };

    enum class OverflowPolicy { RejectNewest, FlushQueued };

    // The queue limit bounds preview latency if a producer outruns playback.
    std::shared_ptr<Handle> createSource(
        const char* name, size_t queueLimitKB = 64,
        OverflowPolicy overflowPolicy = OverflowPolicy::FlushQueued);

    void setGain(const std::shared_ptr<Handle>& handle, float gain) noexcept;
    void clear(const std::shared_ptr<Handle>& handle) noexcept;
    void triggerFadeIn(const std::shared_ptr<Handle>& handle,
                       int durationFrames = kDefaultFadeFrames) noexcept;
    void push(const std::shared_ptr<Handle>& handle, const void* data, int bytes);

    SDL_AudioFormat dev_fmt() const { return SDL_AUDIO_F32; }
    int dev_rate() const { return sourceRate_; }
    int dev_channels() const { return sourceChannels_; }

private:
    AudioBus() = default;
    ~AudioBus();
    AudioBus(const AudioBus&) = delete;
    AudioBus& operator=(const AudioBus&) = delete;

    struct Source {
        mutable std::mutex mutex;
        SDL_AudioStream* stream = nullptr;
        MIX_Track* track = nullptr;
        size_t queueLimitBytes = 0;
        OverflowPolicy overflowPolicy = OverflowPolicy::FlushQueued;
        int fadeTotalFrames = 0;
        int fadeConsumedFrames = 0;

        ~Source();
        void shutdown() noexcept;
    };

    void pushImpl(Source& source, const void* data, int bytes);

    MIX_Mixer* mixer_ = nullptr;
    MusicPlayer* musicPlayer_ = nullptr;
    std::mutex callbackMutex_;
    bool mixerInitialized_ = false;
    uint64_t generation_ = 1;

    mutable std::mutex mtx_;
    std::vector<std::weak_ptr<Source>> sources_;

    // Stable producer format, independent of the current hardware format.
    int sourceRate_ = 48000;
    int sourceChannels_ = 2;
};
