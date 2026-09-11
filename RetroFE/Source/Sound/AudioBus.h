#pragma once
#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>

class MusicPlayer;

class AudioBus {
    struct Source;
public:
    using SourceId = uint32_t;
    static AudioBus& instance();
    // Main-thread lifecycle. Stop external producers before shutdown/reconfiguration.
    bool initialize(int sampleRate = 48000, int channels = 2);
    void shutdown();
    void configureFromMixer(); // Retained for existing callers; initializes if needed.
    MIX_Mixer* mixer() const { return mixer_; }
    uint64_t generation() const { return generation_; }
    void setMusicPlayer(MusicPlayer* player);


    class Handle {
        friend class AudioBus;
        std::shared_ptr<Source> sp;
        explicit Handle(std::shared_ptr<Source> s) : sp(std::move(s)) {}
    };

    // Get a handle for an existing source ID. Returns nullptr if not found.
    std::shared_ptr<Handle> getHandle(SourceId id);

    // Source management
    SourceId addSource(const char* name, size_t ring_buffer_size_kb = 256);
    void removeSource(SourceId id);
    void setEnabled(SourceId id, bool on);
    bool isEnabled(SourceId id) const;
    void push(const std::shared_ptr<Handle>& h, const void* data, int bytes);
    void clear(const std::shared_ptr<Handle>& h) noexcept;

    // <<< MOVE triggerFadeIn BELOW Source definition >>>
    void triggerFadeIn(const std::shared_ptr<Handle>& h, int durationSamples = Source::kMaxFadeSamples) noexcept;

    // Producer/Consumer
    void push(SourceId id, const void* data, int bytes);
    void setGain(const std::shared_ptr<Handle>& h, float gain) noexcept;
    void mixInto(Uint8* dst, int len);
    void mixInto_s16(Uint8* dst, int lenBytes);
    void mixInto_f32(Uint8* dst, int lenBytes);
    void mixInto_s32(Uint8* dst, int lenBytes);

    // Device spec
    SDL_AudioFormat dev_fmt() const { return devFmt_; }
    int dev_rate() const { return devRate_; }
    int dev_channels() const { return devChans_; }
private:
   
    void pushImpl(Source& src, const void* data, int bytes);
    
    class SpscRing {
    public:
        explicit SpscRing(size_t cap_req = (1u << 18), size_t align = 1);
        int write(const uint8_t* data, int bytes);
        int read(uint8_t* out, int bytes);
        void clear();
        // AudioBus.h (inside SpscRing public)
        size_t available() const {
            std::lock_guard<std::mutex> lock(ringMutex_);
            size_t h = head_.load(std::memory_order_acquire);
            size_t t = tail_.load(std::memory_order_relaxed);
            return h - t;
        }

    private:
        mutable std::mutex ringMutex_;
        std::vector<uint8_t> buf_;
        const size_t mask_;
        const size_t align_;          // bytes-per-frame alignment (>=1)
        std::atomic<size_t> head_{ 0 }, tail_{ 0 };
    };

    struct Source {
        std::string name;
        SpscRing ring; // device-format bytes
        std::atomic<bool> enabled{ true };
        std::atomic<float> gain{ 1.0f };
        // NEW: Fade-in state for handling DISCONT
        std::atomic<int> fadeSamplesLeft{ 0 };
        static constexpr int kMaxFadeSamples = 256;  // ~5.3ms at 48kHz — adjustable

        explicit Source(size_t cap, size_t align) : ring(cap, align) {}
    };

    static void SDLCALL postMix(void*, MIX_Mixer*, const SDL_AudioSpec*, float*, int);
    MIX_Mixer* mixer_ = nullptr;
    MusicPlayer* musicPlayer_ = nullptr;
    std::mutex callbackMutex_;
    bool mixerInitialized_ = false;
    uint64_t generation_ = 1;
    AudioBus() = default;
    ~AudioBus();

    // Typedef just to keep lines short
    using SourceVec = std::vector<std::shared_ptr<Source>>;
    using ConstSourceVec = const SourceVec;

    // In AudioBus.h:
    std::shared_ptr<ConstSourceVec> snapshot_;  // plain shared_ptr (not atomic<T>)

    // Inline accessor:
    std::shared_ptr<ConstSourceVec> snapshot() const noexcept {
        return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    }

    // Rebuild snapshot after any change to sources_/enabled flags (expects mtx_ held)
    void rebuildSnapshotLocked();

    mutable std::mutex mtx_; // protects sources_ and control-plane ops
    std::unordered_map<SourceId, std::shared_ptr<Source>> sources_;

    SourceId        nextId_{ 1 };
    SDL_AudioFormat devFmt_{ SDL_AUDIO_S16 };
    int             devRate_{ 48000 };
    int             devChans_{ 2 };
};
