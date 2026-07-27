#ifndef VIZANIM_PROVIDER_H
#define VIZANIM_PROVIDER_H

#include <array>
#include <memory>
#include <vector>
#include <VASDK/GenericAudioDSPProvider.h>
#include <VASDK/CommonAudioUtils.h>
#include "vasdk_vizanim_export.h"

namespace va {
    namespace thirdparty {

        /**
         * @brief VizAnimProvider extracts rich audio features for live animation/visualization.
         *
         * Implements the GenericAudioDSPProvider interface as a pass-through DSP stage:
         * audio bytes are forwarded to the output unchanged while features are updated
         * as a side-effect of each processAudio() call.
         *
         * Features are computed with SIMD acceleration (Google Highway) and FFT
         * via pffft.  All heavy dependencies are hidden behind the pimpl (VizAnimImpl).
         */
        class VASDK_VIZANIM_EXPORT VizAnimProvider : public va::audio::GenericAudioDSPProvider {
        public:
            // the number of chroma bins in the chromagram feature
            static constexpr int NUM_CHROMA = 12;

            // fft size for feature extraction (must be a power of 2, typically 2048)
            static constexpr int FFT_SIZE = 2048;

            /**
             * @brief Per-frame audio features extracted by the provider.
             */
            struct Features {
                float rms;
                float bass, low_mid, mid, hi_mid, treble;
                float centroid;    ///< spectral centroid normalised to [0,1]
                float rolloff;     ///< 85%-energy frequency normalised to [0,1]
                float flux;
                bool  onset;
                float zcr;         ///< zero-crossing rate normalised to [0,1]
                float pitch;       ///< fundamental frequency in Hz (0 = unvoiced)
                float voiced;      ///< YIN confidence [0,1]
                std::array<float, NUM_CHROMA> chroma;
            };

            VizAnimProvider(int fftSize = FFT_SIZE);
            ~VizAnimProvider() override;

            // Non-copyable
            VizAnimProvider(const VizAnimProvider&)            = delete;
            VizAnimProvider& operator=(const VizAnimProvider&) = delete;

            //  GenericAudioDSPProvider 
            std::string getName() const override;

            /**
             * @brief Initialize with an audio configuration.
             * @param config  Must use FLOAT format.  Sample rate is forwarded to the
             *                feature extractor (chroma map, YIN pitch).
             */
            bool initialize(const va::audio::AudioConfig& config) override;
            void release() override;

            /**
             * @brief Pass audio through unchanged and update features as a side-effect.
             *
             * Features are recomputed whenever at least one full analysis window
             * (2048 samples) has been accumulated since the last extraction.
             */
            size_t processAudio(const char* input, char* output,
                                size_t numSamples) override;

            
            /**
             * @brief computes the features from the input audio
             * @param input the audio input
             * @param numSamples the number of samples
             */
            std::vector<Features> computeFeatures(const char* input, int numSamples) const;

        private:
            // Forward-declared pimpl - keeps pffft / Highway headers out of this file.
            class VizAnimImpl;
            std::unique_ptr<VizAnimImpl> _impl;
            int _fftSize;

            mutable std::vector<float>  _sampleBuf;   ///< float sample accumulation buffer
            mutable std::vector<float>  _convertBuf;  ///< scratch for int16→float conversion
        };

    } // namespace thirdparty
} // namespace va

#endif // VIZANIM_PROVIDER_H