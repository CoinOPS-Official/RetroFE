#pragma once

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class MusicPlayer;

class AudioBus {
    struct Source;

public:
    using SourceId = uint32_t;

    // About 5.3 ms at 48 kHz.
    static constexpr int kDefaultFadeFrames = 256;

    static AudioBus& instance();

    // Main-thread lifecycle.
    // External producers should be stopped/quiesced before shutdown.
    bool initialize(
        int sampleRate = 48000,
        int channels = 2);

    void shutdown();

    // Retained for existing callers.
    void configureFromMixer();

    MIX_Mixer* mixer() const {
        return mixer_;
    }

    uint64_t generation() const {
        return generation_;
    }

    void setMusicPlayer(MusicPlayer* player);

    class Handle {
        friend class AudioBus;

        std::shared_ptr<Source> sp;

        explicit Handle(
            std::shared_ptr<Source> source)
            : sp(std::move(source)) {
        }
    };

    std::shared_ptr<Handle>
        getHandle(SourceId id);

    // ---------------------------------------------------------------------
    // Source management
    // ---------------------------------------------------------------------

    enum class OverflowPolicy {
        /*
         * Preserve queued audio and reject the new block.
         * Appropriate for continuous streams where dropping queued
         * audio would be more objectionable than producer backpressure.
         */
        RejectNewest,

        /*
         * On overflow, request that the consumer discard all audio that
         * was queued before this point, and reject the current block.
         *
         * Appropriate for video preview audio, where stale audio/latency
         * is worse than a brief dropout.
         */
        FlushQueued
    };

    SourceId addSource(
        const char* name,
        size_t ringBufferSizeKB = 256,
        OverflowPolicy overflowPolicy =
        OverflowPolicy::FlushQueued);

    void removeSource(SourceId id);

    void setEnabled(
        SourceId id,
        bool enabled);

    bool isEnabled(SourceId id) const;

    void setGain(
        const std::shared_ptr<Handle>& handle,
        float gain) noexcept;

    void clear(
        const std::shared_ptr<Handle>& handle) noexcept;

    void triggerFadeIn(
        const std::shared_ptr<Handle>& handle,
        int durationFrames = kDefaultFadeFrames) noexcept;

    // ---------------------------------------------------------------------
    // Producer side
    //
    // The AudioBus producer contract is always:
    //
    //     native-endian interleaved F32
    //     dev_rate()
    //     dev_channels()
    //
    // GStreamer negotiates exactly this format at its appsink.
    // ---------------------------------------------------------------------

    void push(
        const std::shared_ptr<Handle>& handle,
        const void* data,
        int bytes);

    void push(
        SourceId id,
        const void* data,
        int bytes);

    // ---------------------------------------------------------------------
    // Consumer side
    //
    // SDL3_mixer postmix supplies native F32 samples, so AudioBus mixes
    // directly into that buffer.
    //
    // sampleCount is the number of scalar float samples, not frames.
    // ---------------------------------------------------------------------

    void mixInto(
        float* dst,
        int sampleCount);

    // ---------------------------------------------------------------------
    // Stable producer format
    // ---------------------------------------------------------------------

    SDL_AudioFormat dev_fmt() const {
        return SDL_AUDIO_F32;
    }

    int dev_rate() const {
        return devRate_;
    }

    int dev_channels() const {
        return devChans_;
    }

private:
    AudioBus() = default;
    ~AudioBus();

    AudioBus(const AudioBus&) = delete;
    AudioBus& operator=(const AudioBus&) = delete;

    void pushImpl(
        Source& source,
        const void* data,
        int bytes);

    // ---------------------------------------------------------------------
    // Phase-1 ring implementation
    //
    // This deliberately retains the existing mutex. The producer currently
    // advances tail_ on overflow, so it is not yet a strict ownership-based
    // SPSC ring. That should be addressed separately after the F32 path is
    // verified.
    // ---------------------------------------------------------------------

    class SpscRing {
    public:
        explicit SpscRing(
            size_t requestedCapacity = (1u << 18),
            size_t alignment = 1,
            OverflowPolicy overflowPolicy =
            OverflowPolicy::FlushQueued);

        /*
         * One producer only.
         *
         * Returns the number of bytes accepted.
         * A return value of zero means the complete incoming block
         * was rejected.
         */
        int write(
            const uint8_t* data,
            int bytes) noexcept;

        /*
         * One consumer only.
         *
         * Returns complete aligned frames only.
         */
        int read(
            uint8_t* out,
            int bytes) noexcept;

        /*
         * Thread-safe control operation.
         *
         * Does not modify the consumer-owned tail cursor. Instead it
         * publishes a discard boundary which the consumer applies.
         */
        void clear() noexcept;

        /*
         * Approximate number of playable bytes currently queued.
         */
        size_t available() const noexcept;

        uint64_t overflowCount() const noexcept {
            return overflowCount_.load(
                std::memory_order_relaxed);
        }

        uint64_t droppedBytes() const noexcept {
            return droppedBytes_.load(
                std::memory_order_relaxed);
        }

    private:
        void publishDiscardBefore(
            size_t position) noexcept;

        std::vector<uint8_t> buffer_;

        const size_t mask_;
        const size_t alignment_;
        const OverflowPolicy overflowPolicy_;

        /*
         * Strict ownership:
         *
         * producer:
         *     writes head_
         *
         * consumer:
         *     writes tail_
         *
         * Each side may read the other cursor.
         *
         * Put them on separate cache lines to avoid false sharing between
         * GStreamer's producer thread and SDL's audio thread.
         */
        alignas(64)
            std::atomic<size_t> head_{ 0 };

        alignas(64)
            std::atomic<size_t> tail_{ 0 };

        /*
         * Asynchronous clear/flush request.
         *
         * A producer or control thread publishes a monotonically increasing
         * position. The consumer applies it before its next read.
         */
        alignas(64)
            std::atomic<size_t> discardBefore_{ 0 };

        /*
         * Lets read() notice that a clear occurred while it was copying.
         * In that case the copied block is rejected instead of mixed.
         */
        std::atomic<uint64_t> clearSerial_{ 0 };

        std::atomic<uint64_t> overflowCount_{ 0 };
        std::atomic<uint64_t> droppedBytes_{ 0 };
    };

    // ---------------------------------------------------------------------
    // Fade state
    //
    // Packed into one atomic:
    //
    //     high 32 bits = total fade frames
    //     low  32 bits = consumed fade frames
    //
    // This lets the audio callback advance the fade with CAS without
    // accidentally overwriting a new discontinuity-triggered fade.
    // ---------------------------------------------------------------------

    struct Source {
        std::string name;

        SpscRing ring;

        std::atomic<bool> enabled{ true };
        std::atomic<float> gain{ 1.0f };

        std::atomic<uint64_t> fadeState{ 0 };

        explicit Source(
            size_t capacity,
            size_t alignment,
            OverflowPolicy overflowPolicy)
            : ring(
                capacity,
                alignment,
                overflowPolicy) {
        }
    };

    static void SDLCALL postMix(
        void* userdata,
        MIX_Mixer* mixer,
        const SDL_AudioSpec* spec,
        float* pcm,
        int samples);

    using SourceVec =
        std::vector<std::shared_ptr<Source>>;

    using ConstSourceVec =
        const SourceVec;

    std::shared_ptr<ConstSourceVec>
        snapshot() const noexcept;

    void rebuildSnapshotLocked();

    MIX_Mixer* mixer_ = nullptr;

    // Retained because MusicPlayer currently participates in AudioBus
    // shutdown ordering.
    MusicPlayer* musicPlayer_ = nullptr;
    std::mutex callbackMutex_;

    bool mixerInitialized_ = false;

    uint64_t generation_ = 1;

    std::shared_ptr<ConstSourceVec> snapshot_;

    mutable std::mutex mtx_;

    std::unordered_map<
        SourceId,
        std::shared_ptr<Source>>
        sources_;

    SourceId nextId_{ 1 };

    // The AudioBus producer format is always F32.
    // Only rate/channel count are negotiated from SDL3_mixer.
    int devRate_{ 48000 };
    int devChans_{ 2 };
};