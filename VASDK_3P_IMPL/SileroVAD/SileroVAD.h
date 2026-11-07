#pragma once

#include <VASDK/VADProvider.h>
#include <memory>
#include <string>
#include <vector>
#include "vasdk_silero_vad_export.h"

namespace va::thirdparty {

class VadIterator;

/** Settings of SileroVAD; every field but the model path has a default. */
struct SileroVADOptions {
    /** silero_vad.onnx (downloaded by VASDK_3P_IMPL/cmake/DownloadSileroVADModel.cmake). */
    std::string modelPath;
    /** Window the model scores, in milliseconds: 32 ms is 512 samples at 16 kHz. */
    int windowFrameSizeMs = 32;
    /** Speech probability (0-1) a window needs to count as voice. */
    float threshold = 0.75f;
    /** Silence that ends a speech segment inside one buffer. */
    int minSilenceDurationMs = 100;
    /** Padding added before and after each speech segment. */
    int speechPadMs = 30;
    /** Shorter segments are dropped as clicks. */
    int minSpeechDurationMs = 150;
};

/**
 * @brief Voice activity detection with the Silero VAD ONNX model.
 *
 *   va::thirdparty::SileroVADOptions options;
 *   options.modelPath = modelsDir + "/silero_vad.onnx";
 *   auto vad = std::make_shared<va::thirdparty::SileroVAD>(options);
 *   vad->initialize(audioConfig, bufferSize);
 */
class VASDK_SILERO_VAD_EXPORT SileroVAD : public va::audio::VADProvider {
public:
    explicit SileroVAD(SileroVADOptions options = {});
    ~SileroVAD();
    bool initialize(const va::audio::AudioConfig& audioConfig, int bufferSize) override;
    void release() override;
    std::vector<va::audio::VADProvider::VADResult> detectVoiceActivity(const char* buffer, size_t offset,
                                                                       size_t numSamples) const override;
    void resetState();

private:
    SileroVADOptions _options;
    std::unique_ptr<VadIterator> _vadIterator;
    int _bufferSize = 0;  // retained to allocate local buffer in detectVoiceActivity
};

} // namespace va::thirdparty
