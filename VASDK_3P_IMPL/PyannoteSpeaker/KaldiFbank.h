#ifndef PYANNOTE_KALDI_FBANK_H
#define PYANNOTE_KALDI_FBANK_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <vector>
#include <pffft/pffft.h>

namespace va {
    namespace thirdparty {

        /**
         * @brief Log-mel filterbank matching torchaudio.compliance.kaldi.fbank as called by
         *        pyannote's WeSpeaker ResNet34 front end: 16 kHz, 80 bins, 25 ms / 10 ms frames,
         *        hamming window, dither 0, pre-emphasis 0.97, DC removal, snip_edges, 512-point FFT (pffft).
         *
         * Input samples are expected in int16 range (pyannote scales waveforms by 1 << 15).
         */
        class KaldiFbank {
            public:
            static constexpr int kSampleRate  = 16000;
            static constexpr int kFrameLength = 400;
            static constexpr int kFrameShift  = 160;
            static constexpr int kFftSize     = 512;
            static constexpr int kNumBins     = 80;

            KaldiFbank()
            {
                const double pi = 3.14159265358979323846;
                _window.resize(kFrameLength);
                for (int i = 0; i < kFrameLength; ++i) {
                    _window[i] = 0.54 - 0.46 * std::cos(2.0 * pi * i / (kFrameLength - 1));
                }

                _fftSetup.reset(pffft_new_setup(kFftSize, PFFFT_REAL));

                // Triangular filters in the mel domain over the non-Nyquist bins (Kaldi get_mel_banks).
                auto mel = [](double hz) { return 1127.0 * std::log(1.0 + hz / 700.0); };
                const int numFftBins = kFftSize / 2;
                const double binWidth = static_cast<double>(kSampleRate) / kFftSize;
                const double melLow = mel(20.0);
                const double melHigh = mel(kSampleRate / 2.0);
                const double melDelta = (melHigh - melLow) / (kNumBins + 1);
                _melBanks.assign(static_cast<size_t>(kNumBins) * numFftBins, 0.0);
                for (int b = 0; b < kNumBins; ++b) {
                    const double left = melLow + b * melDelta;
                    const double center = left + melDelta;
                    const double right = center + melDelta;
                    for (int k = 0; k < numFftBins; ++k) {
                        const double m = mel(binWidth * k);
                        const double w = std::min((m - left) / (center - left), (right - m) / (right - center));
                        if (w > 0.0) _melBanks[static_cast<size_t>(b) * numFftBins + k] = w;
                    }
                }
            }

            static size_t numFrames(size_t numSamples)
            {
                return numSamples < kFrameLength ? 0 : 1 + (numSamples - kFrameLength) / kFrameShift;
            }

            /** @return numFrames(numSamples) x kNumBins features, row-major. */
            std::vector<float> compute(const float* samples, size_t numSamples) const
            {
                const size_t frames = numFrames(numSamples);
                const int numFftBins = kFftSize / 2;
                const double eps = std::numeric_limits<float>::epsilon();
                std::vector<float> features(frames * kNumBins);
                std::vector<double> frame(kFrameLength);
                std::vector<double> power(numFftBins);
                // pffft needs SIMD-aligned buffers; in-place transform, stack work area (fine for N = 512).
                std::unique_ptr<float, AlignedDeleter> fftBuf(static_cast<float*>(pffft_aligned_malloc(kFftSize * sizeof(float))));
                float* spectrum = fftBuf.get();

                for (size_t f = 0; f < frames; ++f) {
                    const float* src = samples + f * kFrameShift;
                    double mean = 0.0;
                    for (int i = 0; i < kFrameLength; ++i) mean += src[i];
                    mean /= kFrameLength;
                    for (int i = 0; i < kFrameLength; ++i) frame[i] = src[i] - mean;
                    for (int i = kFrameLength - 1; i > 0; --i) frame[i] -= 0.97 * frame[i - 1];
                    frame[0] -= 0.97 * frame[0];

                    for (int i = 0; i < kFrameLength; ++i) spectrum[i] = static_cast<float>(frame[i] * _window[i]);
                    std::fill(spectrum + kFrameLength, spectrum + kFftSize, 0.0f);
                    pffft_transform_ordered(_fftSetup.get(), spectrum, spectrum, nullptr, PFFFT_FORWARD);
                    // Ordered layout: [DC, Nyquist, re1, im1, re2, im2, ...]; Nyquist is unused (Kaldi drops it).
                    power[0] = static_cast<double>(spectrum[0]) * spectrum[0];
                    for (int k = 1; k < numFftBins; ++k) {
                        const double re = spectrum[2 * k], im = spectrum[2 * k + 1];
                        power[k] = re * re + im * im;
                    }

                    float* out = features.data() + f * kNumBins;
                    for (int b = 0; b < kNumBins; ++b) {
                        const double* bank = _melBanks.data() + static_cast<size_t>(b) * numFftBins;
                        double energy = 0.0;
                        for (int k = 0; k < numFftBins; ++k) energy += bank[k] * power[k];
                        out[b] = static_cast<float>(std::log(std::max(energy, eps)));
                    }
                }
                return features;
            }

            private:
            struct SetupDeleter {
                void operator()(PFFFT_Setup* s) const { if (s) pffft_destroy_setup(s); }
            };
            struct AlignedDeleter {
                void operator()(float* p) const { pffft_aligned_free(p); }
            };

            std::vector<double> _window;
            std::unique_ptr<PFFFT_Setup, SetupDeleter> _fftSetup;  // read-only after construction, shareable across threads
            std::vector<double> _melBanks;  // [kNumBins x kFftSize/2], row-major
        };

    } // namespace thirdparty
} // namespace va

#endif
