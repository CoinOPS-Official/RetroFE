#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>

class AudioBus {
public:
    using SourceId = uint32_t;

    static AudioBus& instance();

    // Call once AFTER SDL_mixer opened the device
    // (e.g., right after Mix_OpenAudio or Mix_OpenAudioDevice)
    void configureFromMixer(); // queries Mix_QuerySpec()

    // Create/remove a producer stream (e.g., “video-audio”)
    // src_* are the format you push (e.g., S16/2ch/48000 from GStreamer)

    SourceId addSource(const char* name, size_t ring_buffer_size_kb = 256);

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
    class SpscRing {
    public:
        explicit SpscRing(size_t cap_pow2 = (1u << 18)) : buf_(cap_pow2), mask_(cap_pow2 - 1) {}
        int write(const uint8_t* data, int bytes) {
            if (!data || bytes <= 0) return 0;
            size_t h = head_.load(std::memory_order_relaxed);
            size_t t = tail_.load(std::memory_order_acquire);
            size_t cap = buf_.size();
            size_t used = h - t;
            if ((size_t)bytes > cap) { // only keep last 'cap' bytes
                data += (bytes - cap);
                bytes = (int)cap;
            }
            // If not enough free, advance tail (drop oldest)
            size_t free = cap - used;
            if ((size_t)bytes > free) {
                size_t need = (size_t)bytes - free;
                tail_.store(t + need, std::memory_order_release);
                t += need;
            }
            for (int i = 0; i < bytes; ++i) buf_[(h + i) & mask_] = data[i];
            head_.store(h + bytes, std::memory_order_release);
            return bytes;
        }
        int read(uint8_t* out, int bytes) {
            if (!out || bytes <= 0) return 0;
            size_t h = head_.load(std::memory_order_acquire);
            size_t t = tail_.load(std::memory_order_relaxed);
            size_t avail = h - t;
            if ((size_t)bytes > avail) bytes = (int)avail;
            for (int i = 0; i < bytes; ++i) out[i] = buf_[(t + i) & mask_];
            tail_.store(t + bytes, std::memory_order_release);
            return bytes;
        }
        void clear() {
            size_t h = head_.load(std::memory_order_relaxed);
            tail_.store(h, std::memory_order_release);
        }
    private:
        std::vector<uint8_t> buf_;
        const size_t mask_;
        std::atomic<size_t> head_{ 0 }, tail_{ 0 };
    };

    struct Source {
        std::string       name;
        SpscRing          ring;                 // holds device-format bytes
        std::atomic<bool> enabled{ true };
        explicit Source(size_t cap = (1 << 18)) : ring(cap) {}
    };

    mutable std::mutex mtx_; // protects the map only
    std::unordered_map<SourceId, std::shared_ptr<Source>> sources_;
    std::vector<Uint8> scratch_;                // mixer scratch (device fmt)

    AudioBus() = default;
    ~AudioBus();

    SourceId nextId_{ 1 };

    SDL_AudioFormat devFmt_{ AUDIO_S16 };
    int             devRate_{ 48000 };
    int             devChans_{ 2 };
};
