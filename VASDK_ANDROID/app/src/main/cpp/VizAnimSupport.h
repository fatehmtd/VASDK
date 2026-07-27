#pragma once

// VizAnim glue: extracts audio features (RMS/band energies) for both the mic
// input and synthesized speech output, and forwards them to Java for the
// waveform/animation UI. Compiled out entirely when VASDK_HAS_VIZANIM isn't
// defined - every symbol here lives behind that guard.

#ifdef VASDK_HAS_VIZANIM

#include "VizAnim/AudioFeaturesFrame.h"
#include "VizAnim/VizAnimFrameProcessor.h"
#include "VizAnim/VizAnimProvider.h"

#include <VASDK/CommonAudioUtils.h>
#include <VASDK/FrameProcessor.h>

#include <jni.h>
#include <memory>
#include <vector>

//  AudioFeatureSinkProcessor
// Sits after VizAnimFrameProcessor in the pipeline and intercepts the
// AudioFeaturesFrames it emits, firing a JNI callback to the Java layer.
// The bridge processor cannot do this because it sits before TTS (upstream of
// VizAnim) and therefore never sees AudioFeaturesFrames flowing downstream.

class AudioFeatureSinkProcessor : public va::pipeline::FrameProcessor {
public:
    std::string getName() const override { return "AudioFeatureSinkProcessor"; }

    void setCallbackTarget(JavaVM *jvm, jweak ref, jmethodID onAudioFeatures);

    bool shouldProcess(std::shared_ptr<va::transport::Frame> /*frame*/) const override {
        return true;
    }

    void processFrame(std::shared_ptr<va::transport::Frame> frame,
                      va::pipeline::FlowDirection direction) override;

private:
    JavaVM *_jvm{nullptr};
    jweak _callbackRef{nullptr};
    jmethodID _onAudioFeatures{nullptr};

    void fireAudioFeatures(const std::shared_ptr<va::thirdparty::AudioFeaturesFrame> &af);
};

//  SynthAudioObserver
// Driven directly from the AAudio hardware callback - features are extracted
// exactly when PCM bytes hit the speaker, giving true animation sync.

class SynthAudioObserver {
public:
    bool initialize(const va::audio::AudioConfig &config, JavaVM *jvm, jweak ref,
                    jmethodID onAudioFeatures);

    void onPlayback(const char *data, size_t bytes);

private:
    using FeatureVec = std::vector<va::thirdparty::VizAnimProvider::Features>;

    void fireAudioFeatures(const FeatureVec &batch);

    std::unique_ptr<va::thirdparty::VizAnimProvider> _provider;
    std::vector<char> _accum;
    size_t _windowBytes{0};
    JavaVM *_jvm{nullptr};
    jweak _callbackRef{nullptr};
    jmethodID _onAudioFeatures{nullptr};
};

#endif // VASDK_HAS_VIZANIM
