#include "AudioBus.h"
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
#include <emmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

AudioBus& AudioBus::instance() {
    static AudioBus bus;
    return bus;
}

AudioBus::~AudioBus() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [_, s] : sources_) {
        if (s.stream) SDL_FreeAudioStream(s.stream);
        s.stream = nullptr;
    }
    sources_.clear();
}

void AudioBus::configureFromMixer() {
    int freq = 48000, chans = 2; Uint16 fmt = AUDIO_S16;
    (void)Mix_QuerySpec(&freq, &fmt, &chans); // if 0, defaults remain
    devFmt_ = fmt; devRate_ = freq; devChans_ = chans;
}

AudioBus::SourceId AudioBus::addSource(const char* name,
    SDL_AudioFormat src_fmt, int src_channels, int src_rate) {
    std::lock_guard<std::mutex> lk(mtx_);
    SourceId id = nextId_++;

    // Default-construct Source in-place (no copy/move of atomic)
    auto [it, inserted] = sources_.try_emplace(id);
    Source& s = it->second;

    s.name = name ? name : "source";
    s.stream = SDL_NewAudioStream(src_fmt, src_channels, src_rate,
        devFmt_, devChans_, devRate_);
    s.enabled.store(true, std::memory_order_release);

    return id;
}

void AudioBus::removeSource(SourceId id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sources_.find(id);
    if (it == sources_.end()) return;

    if (it->second.stream) {
        SDL_FreeAudioStream(it->second.stream);
        it->second.stream = nullptr;
    }
    sources_.erase(it);
}

void AudioBus::setEnabled(SourceId id, bool on) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sources_.find(id);
    if (it != sources_.end()) it->second.enabled.store(on, std::memory_order_release);
}

bool AudioBus::isEnabled(SourceId id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sources_.find(id);
    return (it != sources_.end()) ? it->second.enabled.load(std::memory_order_acquire) : false;
}

void AudioBus::push(SourceId id, const void* data, int bytes) {
    // No global lock on the hot path: only look up the stream once.
    SDL_AudioStream* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = sources_.find(id);
        if (it == sources_.end() || !it->second.enabled.load(std::memory_order_acquire)) return;
        s = it->second.stream;
    }
    if (s && data && bytes > 0) {
        (void)SDL_AudioStreamPut(s, data, bytes);
    }
}

void AudioBus::clear(SourceId id) {
    SDL_AudioStream* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = sources_.find(id);
        if (it == sources_.end()) return;
        s = it->second.stream;
    }
    if (!s) return;

    Uint8 scratch[4096];
    for (;;) {
        const int got = SDL_AudioStreamGet(s, scratch, (int)sizeof(scratch));
        if (got <= 0) break;
    }
}

// Your original, portable C++ implementation serves as a perfect fallback
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
    // Pull per-source converted PCM and mix additively

    thread_local std::vector<Uint8> tmp;
    if ((int)tmp.size() < len) {
        tmp.resize(len);
    }

    // Snapshot current sources (to avoid locking during Get())
    std::vector<SDL_AudioStream*> local;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        local.reserve(sources_.size());
        for (auto& kv : sources_) {
            if (kv.second.enabled.load(std::memory_order_acquire) && kv.second.stream)
                local.push_back(kv.second.stream);
        }
    }

    for (auto* s : local) {
        int got = SDL_AudioStreamGet(s, tmp.data(), len);
        if (got < 0) {
            // Optional: log an error here for debugging
            continue;
        }
        if (got == 0) continue;

        if (devFmt_ == AUDIO_S16) {
            mix_s16_sat(dst, tmp.data(), got);
        }
        else {
            SDL_MixAudioFormat(dst, tmp.data(), devFmt_, got, SDL_MIX_MAXVOLUME);
        }
    }
}
