#pragma once

#include <VASDK/Frame.h>
#include "VizAnimProvider.h"

namespace va {
namespace thirdparty {

/**
 * @brief Source that produced the audio features in an AudioFeaturesFrame.
 */
enum class AudioFeaturesSource {
    MIC,    ///< Microphone / user input (from an AudioFrame)
    SYNTH   ///< TTS synthesis output (from a SynthesisFrame)
};

/**
 * @brief Frame carrying extracted audio features from VizAnimProvider.
 *
 * Emitted by VizAnimFrameProcessor for every analysis window's worth of
 * audio, for both microphone (MIC) and TTS synthesis (SYNTH) sources.
 */
class VASDK_VIZANIM_EXPORT AudioFeaturesFrame : public transport::Frame {
public:
    transport::FrameType getType() const override
    {
        return transport::FrameType::AUDIO_FEATURES;
    }

    std::shared_ptr<transport::Frame> clone() const override
    {
        auto c          = std::make_shared<AudioFeaturesFrame>();
        c->timestamp    = timestamp;
        c->sequenceId   = sequenceId;
        c->frameId      = frameId;
        c->sourceId     = sourceId;
        c->featuresArray     = featuresArray;
        c->source       = source;
        return c;
    }

    std::vector<VizAnimProvider::Features> featuresArray{};
    AudioFeaturesSource       source{ AudioFeaturesSource::MIC };
};

} // namespace thirdparty
} // namespace va
