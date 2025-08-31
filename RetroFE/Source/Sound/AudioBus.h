#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

class AudioBus {
public:
    using SourceId = uint32_t;

    static AudioBus& instance();

    // Call once AFTER SDL_mixer opened the device
    // (e.g., right after Mix_OpenAudio or Mix_OpenAudioDevice)
    void configureFromMixer(); // queries Mix_QuerySpec()

    // Create/remove a producer stream (e.g., “video-audio”)
    // src_* are the format you push (e.g., S16/2ch/48000 from GStreamer)
    SourceId addSource(const char* name,
        SDL_AudioFormat src_fmt, int src_channels, int src_rate);
    void     removeSource(SourceId id);

    // Enable/disable a source (soft mute without tearing down)
    void     setEnabled(SourceId id, bool on);
    bool     isEnabled(SourceId id) const;

    // Producer API (thread-safe, lock-light)
    // Push raw PCM in the source format you registered.
    void push(SourceId id, const void* data, int bytes);


    // Consumer API: call from ONE postmix callback to mix all sources
    // ‘dst/len’ are the device buffer (device fmt/rate/channels).
    void mixInto(Uint8* dst, int len);

    // Optional: drain/clear (e.g., on pause/stop)
    void clear(SourceId id);

    // Device spec (read-only)
    SDL_AudioFormat dev_fmt() const { return devFmt_; }
    int             dev_rate() const { return devRate_; }
    int             dev_channels() const { return devChans_; }

private:
    struct Source {
        std::string       name;
        SDL_AudioStream* stream{ nullptr };   // converts src > device spec
        std::atomic<bool> enabled{ true };
    };

    AudioBus() = default;
    ~AudioBus();

    SourceId nextId_{ 1 };
    mutable std::mutex mtx_; // protects map only (not the streams’ internals)
    std::unordered_map<SourceId, Source> sources_;

    SDL_AudioFormat devFmt_{ AUDIO_S16 };
    int             devRate_{ 48000 };
    int             devChans_{ 2 };
};
