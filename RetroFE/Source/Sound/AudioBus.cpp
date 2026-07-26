#include "AudioBus.h"
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
#include <emmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <memory>
#include <atomic>
#include <algorithm>
#include <cstring>

#include "../Utility/Log.h"

namespace {
    // Helper to find the next power of two.
    // e.g., next_power_of_2(257) -> 512
    // This is essential for the SpscRing's bitmask logic to work correctly.
    size_t next_power_of_2(size_t n) {
        if (n == 0) return 1;
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
#if defined(__LP64__) || defined(_WIN64)
        n |= n >> 32; // Only on 64-bit systems
#endif
        n++;
        return n;
    }
}

static inline int bytes_per_sample(SDL_AudioFormat f) noexcept {
    switch (f) {
        case AUDIO_S8: case AUDIO_U8:                           return 1;
        case AUDIO_S16LSB: case AUDIO_S16MSB:                   return 2;
        case AUDIO_S32LSB: case AUDIO_S32MSB:                   return 4;
        case AUDIO_F32LSB: case AUDIO_F32MSB:                   return 4;
        default:                                                return 2;
    }
}


AudioBus::SpscRing::SpscRing(size_t cap_req, size_t align)
    : buf_(next_power_of_2(cap_req)),
    mask_(buf_.size() - 1),
    align_(align ? align : 1) {
}

int AudioBus::SpscRing::write(const uint8_t* data, int bytes) {
    if (!data || bytes <= 0) return 0;

    const size_t cap = buf_.size();

    // If incoming > capacity, keep only last aligned window
    if ((size_t)bytes > cap) {
        size_t keep = (align_ > 1) ? (cap / align_) * align_ : cap;
        data += (bytes - (int)keep);
        bytes = (int)keep;
    }

    size_t h = head_.load(std::memory_order_relaxed);
    size_t t = tail_.load(std::memory_order_acquire);
    size_t used = h - t;
    size_t free = cap - used;

    if ((size_t)bytes > free) {
        // tail_ belongs exclusively to the consumer. Dropping this producer
        // block preserves SPSC correctness; producer-side tail updates can
        // otherwise overwrite a simultaneous consumer advance.
        return 0;
    }

    size_t idx = h & mask_;
    size_t first = std::min((size_t)bytes, cap - idx);
    std::memcpy(&buf_[idx], data, first);
    if (first < (size_t)bytes) {
        std::memcpy(&buf_[0], data + first, (size_t)bytes - first);
    }

    head_.store(h + (size_t)bytes, std::memory_order_release);
    return bytes;
}

int AudioBus::SpscRing::read(uint8_t* out, int bytes) {
    if (!out || bytes <= 0) return 0;

    const size_t cap = buf_.size();

    if (clearRequested_.exchange(false, std::memory_order_acq_rel)) {
        const size_t h = head_.load(std::memory_order_acquire);
        tail_.store(h, std::memory_order_release);
        return 0;
    }

    size_t h = head_.load(std::memory_order_acquire);
    size_t t = tail_.load(std::memory_order_relaxed);
    size_t avail = h - t;

    if ((size_t)bytes > avail) bytes = (int)avail;

    size_t idx = t & mask_;
    size_t first = std::min((size_t)bytes, cap - idx);
    std::memcpy(out, &buf_[idx], first);
    if (first < (size_t)bytes) {
        std::memcpy(out + first, &buf_[0], (size_t)bytes - first);
    }

    tail_.store(t + (size_t)bytes, std::memory_order_release);
    return bytes;
}

void AudioBus::SpscRing::clear() {
    // Only the consumer mutates tail_. It applies this request at the start of
    // its next read, avoiding a control-thread/consumer lost update.
    clearRequested_.store(true, std::memory_order_release);
}



// ----- helpers -----

static inline float clip1(float v) {
    if (v > 1.0f) return  1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

static inline int16_t clip16(int v) {
    if (v > 32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static inline int32_t clip32(int64_t v) noexcept {
    constexpr int64_t INT32_MAX_V = 2147483647LL;
    constexpr int64_t INT32_MIN_V = -2147483647LL - 1LL;  // avoid unary minus warning
    if (v > INT32_MAX_V) return static_cast<int32_t>(INT32_MAX_V);
    if (v < INT32_MIN_V) return static_cast<int32_t>(INT32_MIN_V);
    return static_cast<int32_t>(v);
}


// Fixed -6 dB headroom for the injected (GStreamer) stream
// Rationale: keeps typical content out of saturation when summed with SDL_mixer output.
constexpr int   kHeadroomShiftS16 = 1;     // >>1  (~0.5x)
constexpr float kHeadroomF32 = 0.5f;  //  -6 dB
constexpr int   kHeadroomShiftS32 = 1;     // >>1  (~0.5x)

AudioBus& AudioBus::instance() {
    static AudioBus bus;
    return bus;
}

AudioBus::~AudioBus() {
    std::lock_guard<std::mutex> lk(mtx_);
    sources_.clear();  // shared_ptr destructors free streams safely
}

void AudioBus::configureFromMixer() {
    int freq = 48000, chans = 2; Uint16 fmt = AUDIO_S16SYS;
    (void)Mix_QuerySpec(&freq, &fmt, &chans);
    devFmt_ = fmt; devRate_ = freq; devChans_ = chans;
}

AudioBus::SourceId AudioBus::addSource(const char* name, size_t ring_kb) {
    std::lock_guard<std::mutex> lock(mtx_);
    const SourceId id = nextId_++;

    const size_t bps = bytes_per_sample(devFmt_);
    const size_t bpf = bps * (size_t)devChans_;                 // bytes per frame
    const size_t cap = (size_t)ring_kb * 1024;

    auto src = std::make_shared<Source>(cap, bpf);              // ? construct with align
    src->name = name ? name : std::string();
    src->enabled.store(true, std::memory_order_relaxed);

    sources_[id] = std::move(src);
    rebuildSnapshotLocked();
    return id;
}

void AudioBus::triggerFadeIn(const std::shared_ptr<Handle>& h, int durationSamples) noexcept {
    if (!h || !h->sp) return;

    auto& fade = h->sp->fadeSamplesLeft;
    int current = fade.load(std::memory_order_relaxed);
    int desired = durationSamples;

    while (current < desired &&
        !fade.compare_exchange_weak(current, desired,
            std::memory_order_release,
            std::memory_order_relaxed)) {
        // retry
    }
}

void AudioBus::removeSource(SourceId id) {
    std::shared_ptr<Source> removed;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = sources_.find(id);
        if (it == sources_.end()) return;
        removed = it->second;
        sources_.erase(it);
        rebuildSnapshotLocked();
    }

    const uint64_t underruns =
        removed->underrunCount.load(std::memory_order_relaxed);
    if (underruns > 0) {
        LOG_DEBUG(
            "AudioBus",
            "Source '" + removed->name + "' had " +
                std::to_string(underruns) + " partial-buffer underruns");
    }
}

void AudioBus::setEnabled(SourceId id, bool on) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = sources_.find(id);
    if (it != sources_.end() && it->second) {
        it->second->enabled.store(on, std::memory_order_relaxed);
        rebuildSnapshotLocked();
    }
}

bool AudioBus::isEnabled(SourceId id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sources_.find(id);
    return (it != sources_.end())
        ? it->second->enabled.load(std::memory_order_acquire)
        : false;
}

void AudioBus::push(const std::shared_ptr<Handle>& h, const void* data, int bytes) {
    if (!h || !h->sp) return;
    if (!data || bytes <= 0) return;

    // enabled is atomic; safe lock-free
    if (!h->sp->enabled.load(std::memory_order_acquire)) return;

    pushImpl(*h->sp, data, bytes);
}

void AudioBus::push(SourceId id, const void* data, int bytes) {
    if (!data || bytes <= 0) return;

    std::shared_ptr<Source> sp;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = sources_.find(id);
        if (it == sources_.end() || !it->second) return;
        sp = it->second;
    }

    if (!sp->enabled.load(std::memory_order_acquire)) return;
    pushImpl(*sp, data, bytes);
}

void AudioBus::pushImpl(Source& src, const void* data, int bytes) {
    // Always work on a copy to apply processing
    static thread_local std::vector<uint8_t> temp_buffer;
    static thread_local size_t tl_temp_hwm_cap = 0;
    static thread_local size_t tl_temp_hwm_size = 0;

    if (temp_buffer.size() < static_cast<size_t>(bytes))
        temp_buffer.resize(static_cast<size_t>(bytes));

    if (temp_buffer.capacity() > tl_temp_hwm_cap || temp_buffer.size() > tl_temp_hwm_size) {
        tl_temp_hwm_cap = std::max(tl_temp_hwm_cap, temp_buffer.capacity());
        tl_temp_hwm_size = std::max(tl_temp_hwm_size, temp_buffer.size());

        LOG_DEBUG("AudioBus",
            "push(): temp_buffer HWM increased: bytes=" + std::to_string(bytes) +
            " size=" + std::to_string(temp_buffer.size()) +
            " cap=" + std::to_string(temp_buffer.capacity()));
    }

    std::memcpy(temp_buffer.data(), data, static_cast<size_t>(bytes));

    // 1) fade
    int fadeLeft = src.fadeSamplesLeft.load(std::memory_order_acquire);
    if (fadeLeft > 0 && devFmt_ == AUDIO_S16SYS) {
        int16_t* samples = reinterpret_cast<int16_t*>(temp_buffer.data());
        int total_samples = bytes / (int)sizeof(int16_t);
        int samples_to_fade = std::min(fadeLeft, total_samples);

        for (int i = 0; i < samples_to_fade; ++i) {
            float fade_gain = 1.0f - (static_cast<float>(fadeLeft - i) / fadeLeft);
            samples[i] = static_cast<int16_t>(samples[i] * fade_gain);
        }

        int newFade = fadeLeft - samples_to_fade;
        src.fadeSamplesLeft.store(newFade > 0 ? newFade : 0, std::memory_order_release);
    }

    // 2) gain
    float sourceGain = src.gain.load(std::memory_order_relaxed);

    if (devFmt_ == AUDIO_S16SYS) {
        int16_t* samples = reinterpret_cast<int16_t*>(temp_buffer.data());
        int num_samples = bytes / (int)sizeof(int16_t);
        for (int i = 0; i < num_samples; ++i) samples[i] = static_cast<int16_t>(samples[i] * sourceGain);
    }
    else if (devFmt_ == AUDIO_F32LSB || devFmt_ == AUDIO_F32MSB) {
        float* samples = reinterpret_cast<float*>(temp_buffer.data());
        int num_samples = bytes / (int)sizeof(float);
        for (int i = 0; i < num_samples; ++i) samples[i] *= sourceGain;
    }
    else if (devFmt_ == AUDIO_S32LSB || devFmt_ == AUDIO_S32MSB) {
        int32_t* samples = reinterpret_cast<int32_t*>(temp_buffer.data());
        int num_samples = bytes / (int)sizeof(int32_t);
        for (int i = 0; i < num_samples; ++i)
            samples[i] = static_cast<int32_t>(static_cast<int64_t>(samples[i]) * sourceGain);
    }

    // 3) limiter
    if (devFmt_ == AUDIO_S16SYS) {
        int16_t* samples = reinterpret_cast<int16_t*>(temp_buffer.data());
        int num_samples = bytes / (int)sizeof(int16_t);

        constexpr int16_t soft_threshold = 28000;
        constexpr float knee = 0.1f;

        for (int i = 0; i < num_samples; ++i) {
            int16_t s = samples[i];
            if (std::abs(s) > soft_threshold) {
                float normalized = s / 32768.0f;
                float sign = (normalized >= 0) ? 1.0f : -1.0f;
                float abs_norm = std::abs(normalized);

                float compressed = sign * (soft_threshold / 32768.0f +
                    (abs_norm - soft_threshold / 32768.0f) * knee);
                samples[i] = static_cast<int16_t>(compressed * 32767.0f);
            }
        }
    }

    src.ring.write(temp_buffer.data(), bytes);
}

void AudioBus::setGain(const std::shared_ptr<Handle>& h, float gain) noexcept {
    if (!h || !h->sp) return;
    h->sp->gain.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_release);
}

void AudioBus::clear(const std::shared_ptr<Handle>& h) noexcept {
    if (!h || !h->sp) return;
    h->sp->ring.clear();
}

static inline void mix_s16_sat_scalar(int16_t* dst, const int16_t* src, int n) {
    for (int i = 0; i < n; ++i) {
        int v = (int)dst[i] + (int)src[i];
        dst[i] = clip16(v);
    }
}

static inline void mix_s16_sat(Uint8* dst_u8, const Uint8* src_u8, int bytes) {
    // Cast pointers once at the beginning
    auto* dst = reinterpret_cast<int16_t*>(dst_u8);
    auto* src = reinterpret_cast<const int16_t*>(src_u8);
    int num_samples = bytes / 2;

    if (num_samples < 64) {
        mix_s16_sat_scalar(dst, src, num_samples);
        return;
    }

#if defined(__AVX2__)
    // --- AVX2 Implementation (Processes 16 samples at a time) ---
    int i = 0;
    // Process the bulk of the data in 16-sample (32-byte) chunks
    for (; i <= num_samples - 16; i += 16) {
        // Load 16 samples from dst and src into 256-bit registers
        __m256i d = _mm256_loadu_si256((__m256i*)(dst + i));
        __m256i s = _mm256_loadu_si256((__m256i*)(src + i));
        // Add with saturation. This single instruction is the magic.
        d = _mm256_adds_epi16(d, s);
        // Store the result back
        _mm256_storeu_si256((__m256i*)(dst + i), d);
    }
    // Handle any remaining samples with the scalar fallback
    if (i < num_samples) {
        mix_s16_sat_scalar(dst + i, src + i, num_samples - i);
    }

#elif defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
    // --- SSE2 Implementation (Processes 8 samples at a time) ---
    int i = 0;
    // Process the bulk of the data in 8-sample (16-byte) chunks
    for (; i <= num_samples - 8; i += 8) {
        // Load 8 samples from dst and src into 128-bit registers
        __m128i d = _mm_loadu_si128((__m128i*)(dst + i));
        __m128i s = _mm_loadu_si128((__m128i*)(src + i));
        // Add with saturation.
        d = _mm_adds_epi16(d, s);
        // Store the result back
        _mm_storeu_si128((__m128i*)(dst + i), d);
    }
    // Handle any remaining samples with the scalar fallback
    if (i < num_samples) {
        mix_s16_sat_scalar(dst + i, src + i, num_samples - i);
    }

#elif defined(__ARM_NEON)
    // --- ARM NEON Implementation (Processes 8 samples at a time) ---
    int i = 0;
    // Process the bulk of the data in 8-sample (16-byte) chunks
    for (; i <= num_samples - 8; i += 8) {
        int16x8_t d = vld1q_s16(dst + i);
        int16x8_t s = vld1q_s16(src + i);
        // Saturating add for signed 16-bit integers
        d = vqaddq_s16(d, s);
        vst1q_s16(dst + i, d);
    }
    // Handle any remaining samples with the scalar fallback
    if (i < num_samples) {
        mix_s16_sat_scalar(dst + i, src + i, num_samples - i);
    }

#else
    // --- Fallback for any other architecture ---
    mix_s16_sat_scalar(dst, src, num_samples);

#endif
}

void AudioBus::mixInto(Uint8* dst, int lenBytes) {
    if (!dst || lenBytes <= 0) return;

    switch (devFmt_) {
        // -------- float32 --------
#if defined(AUDIO_F32LSB)
        case AUDIO_F32LSB:
#endif
#if defined(AUDIO_F32MSB)
        case AUDIO_F32MSB:
#endif
        mixInto_f32(dst, lenBytes);
        break;

        // -------- signed 32-bit int --------
#if defined(AUDIO_S32LSB)
        case AUDIO_S32LSB:
#endif
#if defined(AUDIO_S32MSB)
        case AUDIO_S32MSB:
#endif
        mixInto_s32(dst, lenBytes);
        break;

        // -------- signed 16-bit int --------
#if defined(AUDIO_S16LSB)
        case AUDIO_S16LSB:
#endif
#if defined(AUDIO_S16MSB)
        case AUDIO_S16MSB:
#endif
        mixInto_s16(dst, lenBytes);
        break;

        default:
        // Unknown/unsupported device format: do nothing (avoid corruption).
        break;
    }
}

void AudioBus::mixInto_s16(Uint8* dst, int lenBytes) {
    if (!dst || lenBytes <= 0 || devChans_ <= 0) return;

    const int bps = 2;
    const int bpf = devChans_ * bps;
    const int want = (lenBytes / bpf) * bpf;

    auto snap = snapshot();
    if (!snap || snap->empty() || want <= 0) return;

    static thread_local std::vector<Uint8> scratch;

    if ((int)scratch.size() < want) scratch.resize(want);

    Uint8* tmp = scratch.data();

    for (const auto& src : *snap) {
        const int got = src->ring.read(tmp, want);
        const int gotAligned = (got / bpf) * bpf;

        if (gotAligned <= 0) continue;

        // IMPORTANT: Check for underrun
        if (gotAligned < want) {
            // Fill remainder with silence to prevent garbage/crackling
            std::memset(tmp + gotAligned, 0, want - gotAligned);

            // Never take the logger mutex or perform file I/O from the mixer
            // callback. Control-plane code can inspect this counter later.
            src->underrunCount.fetch_add(1, std::memory_order_relaxed);
        }

        mix_s16_sat(dst, tmp, want);
    }
}
void AudioBus::mixInto_f32(Uint8* dst, int lenBytes) {
    if (!dst || lenBytes <= 0 || devChans_ <= 0) return;

    const int bps = 4, bpf = devChans_ * bps;
    const int want = (lenBytes / bpf) * bpf;

    auto snap = snapshot();
    if (!snap || snap->empty() || want <= 0) return;

    static thread_local std::vector<Uint8> scratch;
    if ((int)scratch.size() < want) scratch.resize(want);
    Uint8* tmp = scratch.data();

    float* D = reinterpret_cast<float*>(dst);
    for (const auto& src : *snap) {
        const int got = src->ring.read(tmp, want);
        const int gotAligned = (got / bpf) * bpf;
        if (gotAligned <= 0) continue;

        const float* S = reinterpret_cast<const float*>(tmp);
        const int n = gotAligned / sizeof(float);
        for (int i = 0; i < n; ++i) {
            float v = D[i] + S[i];
            D[i] = (v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v));
        }
    }
}

void AudioBus::mixInto_s32(Uint8* dst, int lenBytes) {
    if (!dst || lenBytes <= 0 || devChans_ <= 0) return;

    const int bps = 4, bpf = devChans_ * bps;
    const int want = (lenBytes / bpf) * bpf;

    auto snap = snapshot();
    if (!snap || snap->empty() || want <= 0) return;

    static thread_local std::vector<Uint8> scratch;
    if ((int)scratch.size() < want) scratch.resize(want);
    Uint8* tmp = scratch.data();
    int32_t* D = reinterpret_cast<int32_t*>(dst);

    for (const auto& src : *snap) {
        const int got = src->ring.read(tmp, want);
        const int gotAligned = (got / bpf) * bpf;
        if (gotAligned <= 0) continue;

        const int32_t* S = reinterpret_cast<const int32_t*>(tmp);
        const int n = gotAligned / sizeof(int32_t);
        for (int i = 0; i < n; ++i) {
            int64_t d = (int64_t)D[i] + (int64_t)S[i];
            D[i] = clip32(d);
        }
    }
}

std::shared_ptr<AudioBus::Handle> AudioBus::getHandle(SourceId id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sources_.find(id);
    if (it == sources_.end() || !it->second) return nullptr;
    return std::shared_ptr<Handle>(new Handle(it->second));
}

void AudioBus::rebuildSnapshotLocked() {
    auto fresh = std::make_shared<SourceVec>();
    fresh->reserve(sources_.size());
    for (auto& kv : sources_) {
        const auto& sp = kv.second;
        if (sp && sp->enabled.load(std::memory_order_relaxed)) {
            fresh->push_back(sp);
        }
    }
    // Bind to const type so the atomic_store overload matches exactly
    std::shared_ptr<ConstSourceVec> publish = fresh;
    std::atomic_store_explicit(&snapshot_, publish, std::memory_order_release);
}


