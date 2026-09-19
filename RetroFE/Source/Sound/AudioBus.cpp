#include "AudioBus.h"
#include "MusicPlayer.h"

#include "../Utility/Log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>

namespace {

    size_t nextPowerOfTwo(size_t value) {
        if (value == 0)
            return 1;

        --value;

        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;

#if defined(__LP64__) || defined(_WIN64)
        value |= value >> 32;
#endif

        return value + 1;
    }

    size_t alignUp(
        size_t value,
        size_t alignment) {
        if (alignment <= 1)
            return value;

        return
            ((value + alignment - 1) /
                alignment) *
            alignment;
    }

    uint64_t packFadeState(
        uint32_t totalFrames,
        uint32_t consumedFrames) {
        return
            (static_cast<uint64_t>(totalFrames) << 32) |
            static_cast<uint64_t>(consumedFrames);
    }

    uint32_t fadeTotalFrames(
        uint64_t state) {
        return static_cast<uint32_t>(
            state >> 32);
    }

    uint32_t fadeConsumedFrames(
        uint64_t state) {
        return static_cast<uint32_t>(
            state & 0xffffffffULL);
    }

    /*
     * 4096 floats = 16 KiB.
     *
     * Large enough for normal callbacks but mixInto() also chunks larger
     * callbacks, so the audio thread never needs to allocate.
     */
    constexpr int kMixScratchSamples = 4096;

} // namespace

// ============================================================================
// SpscRing
// ============================================================================

AudioBus::SpscRing::SpscRing(
    size_t requestedCapacity,
    size_t alignment,
    OverflowPolicy overflowPolicy)
    : buffer_(
        nextPowerOfTwo(
            std::max<size_t>(
                requestedCapacity,
                1)))
    , mask_(buffer_.size() - 1)
    , alignment_(
        std::max<size_t>(
            alignment,
            1))
    , overflowPolicy_(overflowPolicy) {
}

void AudioBus::SpscRing::publishDiscardBefore(
    size_t position) noexcept {
    /*
     * Multiple control paths may request a clear. Never allow an older
     * request to move the discard point backwards.
     */
    size_t current =
        discardBefore_.load(
            std::memory_order_relaxed);

    while (current < position &&
        !discardBefore_.compare_exchange_weak(
            current,
            position,
            std::memory_order_release,
            std::memory_order_relaxed))
    {
        // current is refreshed by compare_exchange_weak().
    }

    /*
     * Publish after the boundary so the consumer that observes the serial
     * change will also observe the new discardBefore_ value.
     */
    clearSerial_.fetch_add(
        1,
        std::memory_order_release);
}

int AudioBus::SpscRing::write(
    const uint8_t* data,
    int bytes) noexcept {
    if (!data || bytes <= 0)
        return 0;

    /*
     * A producer must never publish a partial audio frame.
     */
    size_t requested =
        static_cast<size_t>(bytes);

    requested -=
        requested %
        alignment_;

    if (requested == 0)
        return 0;

    const size_t capacity =
        buffer_.size();

    /*
     * Never split a producer block merely to make it fit.
     *
     * Normal GStreamer buffers are dramatically smaller than our ring,
     * so this also makes overflow behavior deterministic.
     */
    if (requested > capacity) {
        overflowCount_.fetch_add(
            1,
            std::memory_order_relaxed);

        droppedBytes_.fetch_add(
            requested,
            std::memory_order_relaxed);

        if (overflowPolicy_ ==
            OverflowPolicy::FlushQueued)
        {
            const size_t head =
                head_.load(
                    std::memory_order_relaxed);

            publishDiscardBefore(head);
        }

        return 0;
    }

    /*
     * Producer owns head_.
     *
     * tail_ is acquired because the consumer publishes it after finishing
     * its reads.
     */
    const size_t head =
        head_.load(
            std::memory_order_relaxed);

    const size_t tail =
        tail_.load(
            std::memory_order_acquire);

    const size_t used =
        head - tail;

    /*
     * With monotonically increasing cursors, used should never exceed
     * capacity unless the SPSC ownership contract has been violated.
     */
    if (used > capacity) {
        overflowCount_.fetch_add(
            1,
            std::memory_order_relaxed);

        droppedBytes_.fetch_add(
            requested,
            std::memory_order_relaxed);

        return 0;
    }

    const size_t free =
        capacity - used;

    if (requested > free) {
        overflowCount_.fetch_add(
            1,
            std::memory_order_relaxed);

        droppedBytes_.fetch_add(
            requested,
            std::memory_order_relaxed);

        /*
         * Important:
         *
         * DO NOT advance tail_ here.
         *
         * tail_ belongs exclusively to the consumer. Mutating it from the
         * producer is exactly what prevented the old ring from being truly
         * SPSC.
         */
        if (overflowPolicy_ ==
            OverflowPolicy::FlushQueued)
        {
            /*
             * Low-latency recovery:
             *
             * ask the consumer to throw away everything currently queued.
             *
             * We still reject this block because the consumer may currently
             * be reading from that memory; reclaiming it immediately would
             * introduce a data race.
             */
            publishDiscardBefore(head);
        }

        return 0;
    }

    const size_t index =
        head & mask_;

    const size_t first =
        std::min(
            requested,
            capacity - index);

    std::memcpy(
        buffer_.data() + index,
        data,
        first);

    if (first < requested) {
        std::memcpy(
            buffer_.data(),
            data + first,
            requested - first);
    }

    /*
     * Publishing head_ makes all preceding buffer writes visible to the
     * consumer that acquires head_.
     */
    head_.store(
        head + requested,
        std::memory_order_release);

    return static_cast<int>(
        requested);
}

int AudioBus::SpscRing::read(
    uint8_t* out,
    int bytes) noexcept {
    if (!out || bytes <= 0)
        return 0;

    size_t requested =
        static_cast<size_t>(bytes);

    requested -=
        requested %
        alignment_;

    if (requested == 0)
        return 0;

    /*
     * Snapshot the clear generation before examining cursors.
     *
     * If a clear happens while we're copying, we'll discard the copied
     * block rather than passing stale timeline audio to the mixer.
     */
    const uint64_t clearBefore =
        clearSerial_.load(
            std::memory_order_acquire);

    /*
     * Consumer exclusively owns tail_.
     */
    size_t tail =
        tail_.load(
            std::memory_order_relaxed);

    /*
     * Honor any asynchronous clear request.
     */
    const size_t discard =
        discardBefore_.load(
            std::memory_order_acquire);

    if (discard > tail) {
        tail = discard;

        tail_.store(
            tail,
            std::memory_order_release);
    }

    /*
     * Acquire head_ so all producer memcpy writes preceding its release-store
     * are visible before we read them.
     */
    const size_t head =
        head_.load(
            std::memory_order_acquire);

    const size_t availableBytes =
        head - tail;

    if (availableBytes == 0)
        return 0;

    requested =
        std::min(
            requested,
            availableBytes);

    requested -=
        requested %
        alignment_;

    if (requested == 0)
        return 0;

    const size_t capacity =
        buffer_.size();

    const size_t index =
        tail & mask_;

    const size_t first =
        std::min(
            requested,
            capacity - index);

    std::memcpy(
        out,
        buffer_.data() + index,
        first);

    if (first < requested) {
        std::memcpy(
            out + first,
            buffer_.data(),
            requested - first);
    }

    /*
     * A producer/control thread may have requested a discontinuity flush
     * while we were copying.
     *
     * Don't commit this read and don't return stale samples to AudioBus.
     * The next read will apply the newly published discard boundary.
     */
    const uint64_t clearAfter =
        clearSerial_.load(
            std::memory_order_acquire);

    if (clearAfter != clearBefore)
        return 0;

    /*
     * Consumer commits the read.
     */
    tail_.store(
        tail + requested,
        std::memory_order_release);

    return static_cast<int>(
        requested);
}

void AudioBus::SpscRing::clear() noexcept {
    /*
     * Snapshot everything that has been published by the producer.
     *
     * We intentionally do NOT change tail_. The consumer owns tail_.
     */
    const size_t head =
        head_.load(
            std::memory_order_acquire);

    publishDiscardBefore(head);
}

size_t AudioBus::SpscRing::available() const noexcept {
    const size_t head =
        head_.load(
            std::memory_order_acquire);

    const size_t tail =
        tail_.load(
            std::memory_order_acquire);

    const size_t discard =
        discardBefore_.load(
            std::memory_order_acquire);

    const size_t effectiveTail =
        std::max(
            tail,
            discard);

    if (head <= effectiveTail)
        return 0;

    return head - effectiveTail;
}

// ============================================================================
// AudioBus lifecycle
// ============================================================================

AudioBus& AudioBus::instance() {
    static AudioBus bus;
    return bus;
}

AudioBus::~AudioBus() {
    shutdown();

    std::lock_guard<std::mutex>
        lock(mtx_);

    sources_.clear();
}

bool AudioBus::initialize(
    int sampleRate,
    int channels) {
    if (mixer_)
        return true;

    if (sampleRate <= 0 ||
        channels <= 0 ||
        channels > 8)
    {
        return SDL_SetError(
            "Invalid audio rate or channel count");
    }

    if (!MIX_Init())
        return false;

    mixerInitialized_ = true;

    /*
     * SDL3_mixer mixes internally in float. Request F32 explicitly so our
     * intent is clear even though the postmix callback itself is F32.
     */
    SDL_AudioSpec requested{
        SDL_AUDIO_F32,
        channels,
        sampleRate
    };

    mixer_ =
        MIX_CreateMixerDevice(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
            &requested);

    if (!mixer_) {
        shutdown();
        return false;
    }

    SDL_AudioSpec actual{};

    if (!MIX_GetMixerFormat(
        mixer_,
        &actual))
    {
        shutdown();
        return false;
    }

    /*
     * AudioBus' producer-side format is always F32.
     *
     * Rate and channel count follow SDL3_mixer so GStreamer can negotiate
     * exactly the same stream shape.
     */
    devRate_ =
        actual.freq > 0
        ? actual.freq
        : sampleRate;

    devChans_ =
        actual.channels > 0
        ? actual.channels
        : channels;

    if (!MIX_SetPostMixCallback(
        mixer_,
        postMix,
        this))
    {
        shutdown();
        return false;
    }

    LOG_INFO(
        "AudioBus",
        "Initialized F32 audio bus: " +
        std::to_string(devRate_) +
        " Hz, " +
        std::to_string(devChans_) +
        " channels");

    return true;
}

void AudioBus::shutdown() {
    /*
     * Disconnect the real-time callback first. After this returns,
     * AudioBus state can be torn down without the mixer observing it.
     */
    if (mixer_) {
        MIX_SetPostMixCallback(
            mixer_,
            nullptr,
            nullptr);
    }

    MusicPlayer* player = nullptr;

    {
        std::lock_guard<std::mutex>
            lock(callbackMutex_);

        player = musicPlayer_;
        musicPlayer_ = nullptr;
    }

    if (player)
        player->releaseAudio();

    if (mixer_) {
        MIX_DestroyMixer(mixer_);
        mixer_ = nullptr;
    }

    ++generation_;

    {
        std::lock_guard<std::mutex>
            lock(mtx_);

        for (auto& entry : sources_) {
            if (!entry.second)
                continue;

            entry.second->enabled.store(
                false,
                std::memory_order_release);

            entry.second->ring.clear();
        }

        sources_.clear();

        rebuildSnapshotLocked();
    }

    if (mixerInitialized_)
        MIX_Quit();

    mixerInitialized_ = false;
}

void AudioBus::configureFromMixer() {
    if (!initialize()) {
        LOG_ERROR(
            "AudioBus",
            std::string(
                "Audio initialization failed: ") +
            SDL_GetError());
    }
}

void AudioBus::setMusicPlayer(
    MusicPlayer* player) {
    std::lock_guard<std::mutex>
        lock(callbackMutex_);

    musicPlayer_ = player;
}

// ============================================================================
// SDL3_mixer postmix
// ============================================================================

void SDLCALL AudioBus::postMix(
    void* userdata,
    MIX_Mixer*,
    const SDL_AudioSpec* spec,
    float* pcm,
    int samples) {
    if (!userdata ||
        !spec ||
        !pcm ||
        samples <= 0)
    {
        return;
    }

    auto& bus =
        *static_cast<AudioBus*>(
            userdata);

    /*
     * GStreamer appsinks are created with the AudioBus rate/channel
     * contract. If SDL3_mixer ever changes this contract underneath us,
     * do not reinterpret queued audio incorrectly.
     *
     * Do not log from the real-time callback.
     */
    if (spec->channels !=
        bus.devChans_ ||
        spec->freq !=
        bus.devRate_)
    {
        return;
    }

    bus.mixInto(
        pcm,
        samples);
}

// ============================================================================
// Source control
// ============================================================================

AudioBus::SourceId AudioBus::addSource(
    const char* name,
    size_t ringBufferSizeKB,
    OverflowPolicy overflowPolicy) {
    std::lock_guard<std::mutex>
        lock(mtx_);

    const SourceId id =
        nextId_++;

    const size_t bytesPerFrame =
        sizeof(float) *
        static_cast<size_t>(
            devChans_);

    const size_t capacity =
        std::max<size_t>(
            ringBufferSizeKB * 1024,
            bytesPerFrame);

    auto source =
        std::make_shared<Source>(
            capacity,
            bytesPerFrame,
            overflowPolicy);

    source->name =
        name
        ? name
        : std::string();

    source->enabled.store(
        true,
        std::memory_order_relaxed);

    sources_[id] =
        std::move(source);

    rebuildSnapshotLocked();

    return id;
}

void AudioBus::removeSource(
    SourceId id) {
    std::lock_guard<std::mutex>
        lock(mtx_);

    sources_.erase(id);

    rebuildSnapshotLocked();
}

void AudioBus::setEnabled(
    SourceId id,
    bool enabled) {
    std::lock_guard<std::mutex>
        lock(mtx_);

    auto it =
        sources_.find(id);

    if (it == sources_.end() ||
        !it->second)
    {
        return;
    }

    it->second->enabled.store(
        enabled,
        std::memory_order_release);

    rebuildSnapshotLocked();
}

bool AudioBus::isEnabled(
    SourceId id) const {
    std::lock_guard<std::mutex>
        lock(mtx_);

    auto it =
        sources_.find(id);

    if (it == sources_.end() ||
        !it->second)
    {
        return false;
    }

    return
        it->second->enabled.load(
            std::memory_order_acquire);
}

std::shared_ptr<AudioBus::Handle>
AudioBus::getHandle(
    SourceId id) {
    std::lock_guard<std::mutex>
        lock(mtx_);

    auto it =
        sources_.find(id);

    if (it == sources_.end() ||
        !it->second)
    {
        return nullptr;
    }

    return std::shared_ptr<Handle>(
        new Handle(it->second));
}

void AudioBus::setGain(
    const std::shared_ptr<Handle>& handle,
    float gain) noexcept {
    if (!handle ||
        !handle->sp)
    {
        return;
    }

    handle->sp->gain.store(
        std::clamp(
            gain,
            0.0f,
            1.0f),
        std::memory_order_release);
}

void AudioBus::clear(
    const std::shared_ptr<Handle>& handle)
    noexcept {
    if (!handle ||
        !handle->sp)
    {
        return;
    }

    handle->sp->ring.clear();
}

void AudioBus::triggerFadeIn(
    const std::shared_ptr<Handle>& handle,
    int durationFrames) noexcept {
    if (!handle ||
        !handle->sp)
    {
        return;
    }

    if (durationFrames <= 0) {
        handle->sp->fadeState.store(
            0,
            std::memory_order_release);

        return;
    }

    const uint32_t frames =
        static_cast<uint32_t>(
            std::min<uint64_t>(
                static_cast<uint64_t>(
                    durationFrames),
                static_cast<uint64_t>(
                    std::numeric_limits<uint32_t>::
                    max())));

    /*
     * New discontinuity = restart the fade from frame zero.
     *
     * One atomic contains both total and progress, so the mixer callback
     * cannot accidentally overwrite a newer fade request.
     */
    handle->sp->fadeState.store(
        packFadeState(
            frames,
            0),
        std::memory_order_release);
}

// ============================================================================
// Producer
// ============================================================================

void AudioBus::push(
    const std::shared_ptr<Handle>& handle,
    const void* data,
    int bytes) {
    if (!handle ||
        !handle->sp ||
        !data ||
        bytes <= 0)
    {
        return;
    }

    if (!handle->sp->enabled.load(
        std::memory_order_acquire))
    {
        return;
    }

    pushImpl(
        *handle->sp,
        data,
        bytes);
}

void AudioBus::push(
    SourceId id,
    const void* data,
    int bytes) {
    if (!data ||
        bytes <= 0)
    {
        return;
    }

    std::shared_ptr<Source> source;

    {
        std::lock_guard<std::mutex>
            lock(mtx_);

        auto it =
            sources_.find(id);

        if (it == sources_.end() ||
            !it->second)
        {
            return;
        }

        source =
            it->second;
    }

    if (!source->enabled.load(
        std::memory_order_acquire))
    {
        return;
    }

    pushImpl(
        *source,
        data,
        bytes);
}

void AudioBus::pushImpl(
    Source& source,
    const void* data,
    int bytes) {
    if (!data ||
        bytes <= 0 ||
        devChans_ <= 0)
    {
        return;
    }

    const int bytesPerFrame =
        devChans_ *
        static_cast<int>(
            sizeof(float));

    /*
     * Never write a partial interleaved frame.
     */
    bytes -=
        bytes %
        bytesPerFrame;

    if (bytes <= 0)
        return;

    /*
     * No copy, conversion, gain, fade or limiter here.
     *
     * GStreamer already negotiated F32 in exactly our rate/channel layout.
     * Gain and fade are playback-time properties, so they belong on the
     * consumer side.
     */
    source.ring.write(
        static_cast<const uint8_t*>(
            data),
        bytes);
}

// ============================================================================
// Consumer
// ============================================================================

void AudioBus::mixInto(
    float* dst,
    int sampleCount) {
    if (!dst ||
        sampleCount <= 0 ||
        devChans_ <= 0)
    {
        return;
    }

    /*
     * Only process complete interleaved frames.
     */
    const int totalFrames =
        sampleCount /
        devChans_;

    if (totalFrames <= 0)
        return;

    const int totalSamples =
        totalFrames *
        devChans_;

    auto sources =
        snapshot();

    if (!sources ||
        sources->empty())
    {
        return;
    }

    /*
     * Fixed scratch storage:
     *
     * no resize(), no vector allocation and no logging from the
     * real-time callback.
     */
    std::array<
        float,
        kMixScratchSamples>
        scratch{};

    int outputSampleOffset = 0;

    while (outputSampleOffset <
        totalSamples)
    {
        int chunkSamples =
            std::min(
                kMixScratchSamples,
                totalSamples -
                outputSampleOffset);

        /*
         * Keep the chunk aligned to complete frames.
         */
        chunkSamples -=
            chunkSamples %
            devChans_;

        if (chunkSamples <= 0)
            break;

        const int chunkBytes =
            chunkSamples *
            static_cast<int>(
                sizeof(float));

        for (const auto& source :
            *sources)
        {
            if (!source)
                continue;

            if (!source->enabled.load(
                std::memory_order_acquire))
            {
                continue;
            }

            const int gotBytes =
                source->ring.read(
                    reinterpret_cast<uint8_t*>(
                        scratch.data()),
                    chunkBytes);

            if (gotBytes <= 0)
                continue;

            int gotSamples =
                gotBytes /
                static_cast<int>(
                    sizeof(float));

            /*
             * read() is frame-aligned, but retain this defensive rounding.
             */
            gotSamples -=
                gotSamples %
                devChans_;

            if (gotSamples <= 0)
                continue;

            const int gotFrames =
                gotSamples /
                devChans_;

            const float sourceGain =
                source->gain.load(
                    std::memory_order_relaxed);

            /*
             * Snapshot one coherent fade state.
             *
             * If another thread triggers a new fade while this chunk is
             * being mixed, our final CAS fails instead of overwriting the
             * newer request.
             */
            uint64_t fadeState =
                source->fadeState.load(
                    std::memory_order_acquire);

            const uint32_t fadeTotal =
                fadeTotalFrames(
                    fadeState);

            uint32_t fadeConsumed =
                fadeConsumedFrames(
                    fadeState);

            for (int frame = 0;
                frame < gotFrames;
                ++frame)
            {
                float frameGain =
                    sourceGain;

                if (fadeTotal > 0 &&
                    fadeConsumed <
                    fadeTotal)
                {
                    float fadeGain;

                    if (fadeTotal <= 1) {
                        fadeGain = 1.0f;
                    }
                    else {
                        /*
                         * First frame = 0.0
                         * Last fade frame = 1.0
                         */
                        fadeGain =
                            static_cast<float>(
                                fadeConsumed) /
                            static_cast<float>(
                                fadeTotal - 1);
                    }

                    frameGain *=
                        fadeGain;

                    ++fadeConsumed;
                }

                const int sourceBase =
                    frame *
                    devChans_;

                const int destBase =
                    outputSampleOffset +
                    sourceBase;

                for (int channel = 0;
                    channel < devChans_;
                    ++channel)
                {
                    dst[
                        destBase +
                            channel] +=
                        scratch[
                            sourceBase +
                                channel] *
                            frameGain;
                }
            }

            if (fadeTotal > 0) {
                uint64_t desiredFadeState;

                if (fadeConsumed >=
                    fadeTotal)
                {
                    desiredFadeState = 0;
                }
                else {
                    desiredFadeState =
                        packFadeState(
                            fadeTotal,
                            fadeConsumed);
                }

                /*
                 * Only advance the fade we actually consumed.
                 *
                 * If triggerFadeIn() published a newer state during this
                 * chunk, leave that newer state alone.
                 */
                source->fadeState.
                    compare_exchange_strong(
                        fadeState,
                        desiredFadeState,
                        std::memory_order_release,
                        std::memory_order_relaxed);
            }
        }

        /*
         * Final limiter.
         *
         * Do not pre-limit individual sources. Floating point mixing can
         * safely exceed +/-1 internally; clamp only after all injected
         * sources have been accumulated into SDL3_mixer's buffer.
         */
        for (int i = 0;
            i < chunkSamples;
            ++i)
        {
            const int index =
                outputSampleOffset + i;

            dst[index] =
                std::clamp(
                    dst[index],
                    -1.0f,
                    1.0f);
        }

        outputSampleOffset +=
            chunkSamples;
    }
}

// ============================================================================
// Snapshot
// ============================================================================

std::shared_ptr<
    AudioBus::ConstSourceVec>
    AudioBus::snapshot() const noexcept {
    return std::atomic_load_explicit(
        &snapshot_,
        std::memory_order_acquire);
}

void AudioBus::rebuildSnapshotLocked() {
    auto fresh =
        std::make_shared<SourceVec>();

    fresh->reserve(
        sources_.size());

    for (auto& entry : sources_) {
        const auto& source =
            entry.second;

        if (!source)
            continue;

        if (!source->enabled.load(
            std::memory_order_relaxed))
        {
            continue;
        }

        fresh->push_back(
            source);
    }

    std::shared_ptr<
        ConstSourceVec>
        publish = fresh;

    std::atomic_store_explicit(
        &snapshot_,
        std::move(publish),
        std::memory_order_release);
}