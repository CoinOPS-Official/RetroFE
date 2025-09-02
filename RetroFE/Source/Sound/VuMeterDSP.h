// VUMeterDSP.h

#pragma once
#include <vector>
#include <cstdint>

// Forward declarations for SDL types if needed, or include the header
using Uint8 = uint8_t;

class VUMeterDSP {
public:
    VUMeterDSP();

    // Rebuilds coefficients and resizes buffers if config changes
    void rebuild(int barsPerChan, int sampleRate, float fLow, float fHigh);

    // Processes a block of interleaved PCM data
    void process(const Uint8* pcmData, int frameCount, int channelCount, int bytesPerSample);

    // Accessors for the results (linear envelope values)
    const std::vector<float>& getLeftEnvFast() const { return L_.envFast; }
    const std::vector<float>& getLeftEnvSlow() const { return L_.envSlow; }
    const std::vector<float>& getRightEnvFast() const { return R_.envFast; }
    const std::vector<float>& getRightEnvSlow() const { return R_.envSlow; }

private:
    // The core DSP logic, implemented with platform-specific SIMD
    void step(float xl, float xr);

    // Structure of Arrays (SoA) layout for maximum SIMD performance
    struct BandSoA {
        std::vector<float> xPrev, hp, lp;
        std::vector<float> envFast, envSlow;
        std::vector<float> aHP, aLP;
        std::vector<float> kAF, kRF, kAS, kRS;

        void resize(size_t n) {
            xPrev.assign(n, 0.f); hp.assign(n, 0.f); lp.assign(n, 0.f);
            envFast.assign(n, 0.f); envSlow.assign(n, 0.f);
            aHP.resize(n); aLP.resize(n);
            kAF.resize(n); kRF.resize(n); kAS.resize(n); kRS.resize(n);
        }
    };

    BandSoA L_, R_;

    // Cached state to detect when a rebuild is needed
    int cachedBars_ = -1;
    int cachedSR_ = -1;
    int barsPerChan_ = 0;
    int paddedBars_ = 0; // Padded size for safe SIMD operations
};