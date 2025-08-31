#include "AudioBus.h"
#include <algorithm>

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

static inline void mix_s16_sat(Uint8* dst, const Uint8* src, int bytes) {
    auto* d = reinterpret_cast<int16_t*>(dst);
    auto* s = reinterpret_cast<const int16_t*>(src);
    const int n = bytes / 2;
    for (int i = 0; i < n; ++i) {
        int v = int(d[i]) + int(s[i]);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        d[i] = static_cast<int16_t>(v);
    }
}

void AudioBus::mixInto(Uint8* dst, int len) {
    // Pull per-source converted PCM and mix additively

    // Use a static buffer to avoid reallocation every callback
    static std::vector<Uint8> tmp;
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
