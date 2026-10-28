#ifndef PYANNOTE_EMBEDDING_PROVIDER_H
#define PYANNOTE_EMBEDDING_PROVIDER_H

#include <memory>
#include <string>
#include <vector>
#include <VASDK/SpeakerEmbeddingProvider.h>
#include "vasdk_pyannote_speaker_export.h"

namespace va {
    namespace thirdparty {

        /**
         * @brief Speaker embedding provider running the embedding model of
         *        pyannote/speaker-diarization-3.1 (pyannote/wespeaker-voxceleb-resnet34-LM)
         *        exported to ONNX: 80-bin Kaldi fbank in, 256-dim voice print out.
         *
         * Usage:
         *   auto provider = std::make_shared<PyannoteEmbeddingProvider>(modelPath);
         *   provider->initialize(audioConfig);
         *   auto speakerProc = std::make_shared<SpeakerIdentificationProcessor>(provider);
         *   speakerProc->setRingBuffer(ringBuffer);
         */
        class VASDK_PYANNOTE_SPEAKER_EXPORT PyannoteEmbeddingProvider : public va::audio::SpeakerEmbeddingProvider {
        public:
            explicit PyannoteEmbeddingProvider(const std::string& modelPath);
            ~PyannoteEmbeddingProvider() override;

            /**
             * @brief Load the model. Requires 16 kHz mono audio.
             */
            bool initialize(const va::audio::AudioConfig& audioConfig);

            std::vector<float> computeEmbedding(const float* samples, size_t numSamples) override;

        private:
            struct Impl;
            std::unique_ptr<Impl> _impl;
            std::string _modelPath;
        };

    } // namespace thirdparty
} // namespace va

#endif
