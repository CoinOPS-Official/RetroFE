#include "AudioBus.h"
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
#include <emmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

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

AudioBus& AudioBus::instance() {
    static AudioBus bus;
    return bus;
}

AudioBus::~AudioBus() {
    std::lock_guard<std::mutex> lk(mtx_);
    sources_.clear();  // shared_ptr destructors free streams safely
}

void AudioBus::configureFromMixer() {
    int freq = 48000, chans = 2; Uint16 fmt = AUDIO_S16;
    (void)Mix_QuerySpec(&freq, &fmt, &chans); // if 0, defaults remain
    devFmt_ = fmt; devRate_ = freq; devChans_ = chans;
}

AudioBus::SourceId AudioBus::addSource(const char* name, size_t ring_buffer_kb) {
    std::lock_guard<std::mutex> lk(mtx_);

    // 1. Calculate the required buffer capacity.
    // Clamp to a reasonable minimum (e.g., 4KB) and find the next power of two.
    const size_t requested_bytes = std::max((size_t)4, ring_buffer_kb) * 1024;
    const size_t capacity_p2 = next_power_of_2(requested_bytes);

    // 2. Create and configure the new source.
    SourceId id = nextId_++;
    auto sp = std::make_shared<Source>(capacity_p2);
    sp->name = name ? name : "source";
    sp->enabled.store(true, std::memory_order_release);

    // 3. Add it to the map.
    sources_.emplace(id, std::move(sp));

    return id;
}

void AudioBus::removeSource(SourceId id) {
    std::lock_guard<std::mutex> lk(mtx_);
    sources_.erase(id); // freed when last ref is gone
}

void AudioBus::setEnabled(SourceId id, bool on) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sources_.find(id);
    if (it != sources_.end())
        it->second->enabled.store(on, std::memory_order_release);
}

bool AudioBus::isEnabled(SourceId id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sources_.find(id);
    return (it != sources_.end())
        ? it->second->enabled.load(std::memory_order_acquire)
        : false;
}

void AudioBus::push(SourceId id, const void* data, int bytes) {
    if (!data || bytes <= 0) return;
    std::shared_ptr<Source> sp;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = sources_.find(id);
        if (it == sources_.end()) return;
        sp = it->second;
    }
    if (!sp->enabled.load(std::memory_order_acquire)) return;

    // Write; if ring is full, we drop tail (oldest) by clearing a bit or just accept truncation.
    // Simple approach: write what fits (write returns actual bytes written).
    (void)sp->ring.write(reinterpret_cast<const uint8_t*>(data), bytes);
}

void AudioBus::clear(SourceId id) {
    std::shared_ptr<Source> sp;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = sources_.find(id);
        if (it == sources_.end()) return;
        sp = it->second;
    }
    sp->ring.clear();
}

static inline void mix_s16_sat_scalar(int16_t* dst, const int16_t* src, int num_samples) {
    for (int i = 0; i < num_samples; ++i) {
        // Promote to int to avoid overflow before saturation
        int v = (int)dst[i] + (int)src[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        dst[i] = (int16_t)v;
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

void AudioBus::mixInto(Uint8* dst, int len) {
    if (!dst || len <= 0) return;
    if ((int)scratch_.size() < len) scratch_.resize(len);

    std::vector<std::shared_ptr<Source>> local;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        local.reserve(sources_.size());
        for (auto& kv : sources_) {
            const auto& sp = kv.second;
            if (sp->enabled.load(std::memory_order_acquire))
                local.push_back(sp);
        }
    }

    // Device spec (we normalized to S16/48k/2ch)
    const int bps = 2; // S16
    const int bpf = bps * std::max(1, devChans_); // typically 4
    // do NOT memset(dst) in postmix; we add to existing buffer

    for (auto& sp : local) {
        int got = sp->ring.read(scratch_.data(), len);
        if (got <= 0) continue;
        got -= (got % bpf);           // frame-align
        if (got <= 0) continue;

        // S16 additive mix with saturation
        mix_s16_sat(dst, scratch_.data(), got);
    }
}