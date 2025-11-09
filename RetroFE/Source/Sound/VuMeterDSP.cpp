
#include "VuMeterDSP.h"
#include <cmath>
#include <algorithm>

// Include headers for SIMD intrinsics
#if defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
#include <emmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace {
    // Determine SIMD vector width (4 floats for both SSE and NEON)
    constexpr int SIMD_WIDTH = 4;
}

VUMeterDSP::VUMeterDSP() = default;

void VUMeterDSP::rebuild(int barsPerChan, int sampleRate, float fLow, float fHigh) {
    if (cachedBars_ == barsPerChan && cachedSR_ == sampleRate) {
        return; // No change
    }

    barsPerChan_ = barsPerChan;
    // Pad the number of bars to be a multiple of SIMD width for safe vectorization
    paddedBars_ = (barsPerChan_ + SIMD_WIDTH - 1) / SIMD_WIDTH * SIMD_WIDTH;

    L_.resize(paddedBars_);
    R_.resize(paddedBars_);

    // --- The rest of your rebuild logic, now filling the SoA structure ---
    // (This code is adapted from your original `rebuild` lambda)

    // log-spaced edges
    std::vector<float> edge(barsPerChan_ + 1);
    for (int i = 0; i <= barsPerChan_; ++i) {
        float t = float(i) / float(barsPerChan_);
        edge[i] = fLow * powf(fHigh / fLow, t);
    }

    auto fractf = [](float x) { return x - floorf(x); };
    auto hashJitter = [&](int idx) { // deterministic 0.9..1.1
        float s = sinf((idx + 1) * 12.9898f) * 43758.5453f;
        return 0.9f + 0.2f * fractf(s);
        };

    auto k = [&](float ms) { return 1.0f - expf(-1.0f / (ms * 0.001f * float(sampleRate))); };

    for (int b = 0; b < barsPerChan_; ++b) {
        float lo = edge[b], hi = edge[b + 1];

        // 1-pole shapers (HP then LP) for a coarse band-pass
        float aHP = expf(-2.0f * 3.1415926535f * lo / float(sampleRate));
        float aLP = expf(-2.0f * 3.1415926535f * hi / float(sampleRate));

        // Per-band time constants (lows slower, highs faster) + small jitter
        float bandT = (float(b) + 0.5f) / float(barsPerChan_);
        float j = hashJitter(b);

        float aF_ms = 3.0f * j;
        float rF_ms = (40.0f - 20.0f * bandT) * j;
        float aS_ms = (14.0f + 6.0f * bandT) * j;
        float rS_ms = (280.0f - 120.0f * bandT) * j;

        // Write to both L and R channels' SoA data
        L_.aHP[b] = R_.aHP[b] = aHP;
        L_.aLP[b] = R_.aLP[b] = aLP;
        L_.kAF[b] = R_.kAF[b] = k(aF_ms); L_.kRF[b] = R_.kRF[b] = k(rF_ms);
        L_.kAS[b] = R_.kAS[b] = k(aS_ms); L_.kRS[b] = R_.kRS[b] = k(rS_ms);
    }

    cachedBars_ = barsPerChan;
    cachedSR_ = sampleRate;
}

// The core SIMD-optimized processing function
void VUMeterDSP::step(float xl, float xr) {
    auto process_channel_simd = [&](float x, BandSoA& B) {
        int b = 0;

#if defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64))
        const __m128 x_vec = _mm_set1_ps(x);
        const __m128 one_vec = _mm_set1_ps(1.0f);
        const __m128 sign_mask = _mm_set1_ps(-0.0f);

        for (; b <= paddedBars_ - SIMD_WIDTH; b += SIMD_WIDTH) {
            // Load state for 4 bands (SAFE: Using unaligned loads)
            __m128 xPrev_vec = _mm_loadu_ps(&B.xPrev[b]);
            __m128 hp_vec = _mm_loadu_ps(&B.hp[b]);
            __m128 lp_vec = _mm_loadu_ps(&B.lp[b]);
            __m128 ef_vec = _mm_loadu_ps(&B.envFast[b]);
            __m128 es_vec = _mm_loadu_ps(&B.envSlow[b]);

            // Load coefficients for 4 bands
            const __m128 aHP_vec = _mm_loadu_ps(&B.aHP[b]);
            const __m128 aLP_vec = _mm_loadu_ps(&B.aLP[b]);
            const __m128 kAF_vec = _mm_loadu_ps(&B.kAF[b]);
            const __m128 kRF_vec = _mm_loadu_ps(&B.kRF[b]);
            const __m128 kAS_vec = _mm_loadu_ps(&B.kAS[b]);
            const __m128 kRS_vec = _mm_loadu_ps(&B.kRS[b]);

            // --- DSP Processing ---
            // HP Filter
            __m128 new_hp_vec = _mm_mul_ps(aHP_vec, _mm_sub_ps(_mm_add_ps(hp_vec, x_vec), xPrev_vec));
            _mm_storeu_ps(&B.xPrev[b], x_vec); // SAFE: Using unaligned store
            _mm_storeu_ps(&B.hp[b], new_hp_vec);

            // LP Filter
            __m128 term1 = _mm_mul_ps(_mm_sub_ps(one_vec, aLP_vec), new_hp_vec);
            __m128 new_lp_vec = _mm_add_ps(term1, _mm_mul_ps(aLP_vec, lp_vec));
            _mm_storeu_ps(&B.lp[b], new_lp_vec);

            // Rectifier
            __m128 rect_vec = _mm_andnot_ps(sign_mask, new_lp_vec);

            // --- Envelope Followers ---
            // Fast
            __m128 cmp_f_mask = _mm_cmpgt_ps(rect_vec, ef_vec);
            __m128 k_f_vec = _mm_or_ps(_mm_and_ps(cmp_f_mask, kAF_vec), _mm_andnot_ps(cmp_f_mask, kRF_vec));
            __m128 new_ef_vec = _mm_add_ps(ef_vec, _mm_mul_ps(_mm_sub_ps(rect_vec, ef_vec), k_f_vec));
            _mm_storeu_ps(&B.envFast[b], new_ef_vec);

            // Slow
            __m128 cmp_s_mask = _mm_cmpgt_ps(rect_vec, es_vec);
            __m128 k_s_vec = _mm_or_ps(_mm_and_ps(cmp_s_mask, kAS_vec), _mm_andnot_ps(cmp_s_mask, kRS_vec));
            __m128 new_es_vec = _mm_add_ps(es_vec, _mm_mul_ps(_mm_sub_ps(rect_vec, es_vec), k_s_vec));
            _mm_storeu_ps(&B.envSlow[b], new_es_vec);
        }

#elif defined(__ARM_NEON)
        const float32x4_t x_vec = vdupq_n_f32(x);
        const float32x4_t one_vec = vdupq_n_f32(1.0f);

        for (; b <= paddedBars_ - SIMD_WIDTH; b += SIMD_WIDTH) {
            // Load state
            float32x4_t xPrev_vec = vld1q_f32(&B.xPrev[b]);
            float32x4_t hp_vec = vld1q_f32(&B.hp[b]);
            float32x4_t lp_vec = vld1q_f32(&B.lp[b]);
            float32x4_t ef_vec = vld1q_f32(&B.envFast[b]);
            float32x4_t es_vec = vld1q_f32(&B.envSlow[b]);

            // Load coeffs
            const float32x4_t aHP_vec = vld1q_f32(&B.aHP[b]);
            const float32x4_t aLP_vec = vld1q_f32(&B.aLP[b]);
            const float32x4_t kAF_vec = vld1q_f32(&B.kAF[b]);
            const float32x4_t kRF_vec = vld1q_f32(&B.kRF[b]);
            const float32x4_t kAS_vec = vld1q_f32(&B.kAS[b]);
            const float32x4_t kRS_vec = vld1q_f32(&B.kRS[b]);

            // HP Filter
            float32x4_t new_hp_vec = vmulq_f32(aHP_vec, vsubq_f32(vaddq_f32(hp_vec, x_vec), xPrev_vec));
            vst1q_f32(&B.xPrev[b], x_vec);
            vst1q_f32(&B.hp[b], new_hp_vec);

            // LP Filter (using multiply-subtract variant for FMA-like pattern)
            float32x4_t term1 = vmulq_f32(aLP_vec, vsubq_f32(lp_vec, new_hp_vec));
            float32x4_t new_lp_vec = vaddq_f32(new_hp_vec, term1);
            vst1q_f32(&B.lp[b], new_lp_vec);

            // Rectifier
            float32x4_t rect_vec = vabsq_f32(new_lp_vec);

            // Envelope Followers (using bitwise select)
            // Fast
            uint32x4_t cmp_f_mask = vcgtq_f32(rect_vec, ef_vec);
            float32x4_t k_f_vec = vbslq_f32(cmp_f_mask, kAF_vec, kRF_vec);
            float32x4_t new_ef_vec = vaddq_f32(ef_vec, vmulq_f32(vsubq_f32(rect_vec, ef_vec), k_f_vec));
            vst1q_f32(&B.envFast[b], new_ef_vec);

            // Slow
            uint32x4_t cmp_s_mask = vcgtq_f32(rect_vec, es_vec);
            float32x4_t k_s_vec = vbslq_f32(cmp_s_mask, kAS_vec, kRS_vec);
            float32x4_t new_es_vec = vaddq_f32(es_vec, vmulq_f32(vsubq_f32(rect_vec, es_vec), k_s_vec));
            vst1q_f32(&B.envSlow[b], new_es_vec);
        }
#endif

        // --- Scalar fallback for the remainder ---
        for (; b < barsPerChan_; ++b) {
            float hp = B.aHP[b] * (B.hp[b] + x - B.xPrev[b]);
            B.xPrev[b] = x; B.hp[b] = hp;
            float lp = (1.0f - B.aLP[b]) * hp + B.aLP[b] * B.lp[b];
            B.lp[b] = lp;

            float rect = fabsf(lp);

            float& ef = B.envFast[b], & es = B.envSlow[b];
            ef += (rect - ef) * ((rect > ef) ? B.kAF[b] : B.kRF[b]);
            es += (rect - es) * ((rect > es) ? B.kAS[b] : B.kRS[b]);
        }
        };

    process_channel_simd(xl, L_);
    process_channel_simd(xr * 0.997f, R_);
}


void VUMeterDSP::process(const Uint8* p, int frameCount, int chans, int bps) {
    if (bps <= 0 || !p) return;

    auto sampleAt = [&](int frame, int chan)->float {
        const int pos = (frame * chans + chan) * bps;
        if (bps == 2) { int16_t v = *reinterpret_cast<const int16_t*>(p + pos); return float(v) / 32768.f; }
        if (bps == 1) { uint8_t v = *(p + pos); return (float(v) - 128.f) / 128.f; }
        float v = *reinterpret_cast<const float*>(p + pos); return std::clamp(v, -1.f, 1.f);
        };

    for (int i = 0; i < frameCount; ++i) {
        const float xl = sampleAt(i, 0);
        const float xr = (chans > 1) ? sampleAt(i, 1) : xl;
        step(xl, xr);
    }
}