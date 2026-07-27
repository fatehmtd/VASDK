#include "VizAnimSupport.h"

#ifdef VASDK_HAS_VIZANIM

#include <spdlog/spdlog.h>

//  AudioFeatureSinkProcessor

void AudioFeatureSinkProcessor::setCallbackTarget(JavaVM *jvm, jweak ref,
                                                  jmethodID onAudioFeatures) {
    _jvm = jvm;
    _callbackRef = ref;
    _onAudioFeatures = onAudioFeatures;
}

void AudioFeatureSinkProcessor::processFrame(std::shared_ptr<va::transport::Frame> frame,
                                             va::pipeline::FlowDirection direction) {
    if (direction == va::pipeline::FlowDirection::DOWNSTREAM &&
        frame->getType() == va::transport::FrameType::AUDIO_FEATURES) {
        auto af = std::dynamic_pointer_cast<va::thirdparty::AudioFeaturesFrame>(frame);
        if (af) fireAudioFeatures(af);
    }
    if (direction == va::pipeline::FlowDirection::UPSTREAM)
        pushUpstream(frame);
    else
        pushDownstream(frame);
}

void AudioFeatureSinkProcessor::fireAudioFeatures(
        const std::shared_ptr<va::thirdparty::AudioFeaturesFrame> &af) {
    if (!_jvm || !_callbackRef || !_onAudioFeatures) return;
    if (af->featuresArray.empty()) return;
    JNIEnv *env = nullptr;
    bool didAttach = false;
    const jint rc = _jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, "vasdk-vizanim", nullptr};
        if (_jvm->AttachCurrentThread(&env, &args) != JNI_OK || !env) return;
        didAttach = true;
    } else if (rc != JNI_OK || !env) {
        return;
    }
    const char *src = (af->source == va::thirdparty::AudioFeaturesSource::SYNTH) ? "SYNTH"
                                                                                 : "MIC";
    // Pack all features into a flat float array: [rms, bass, lowMid, mid, hiMid, treble, ...]
    static constexpr int kStride = 6;
    const int n = static_cast<int>(af->featuresArray.size());
    jfloatArray jArr = env->NewFloatArray(n * kStride);
    if (jArr) {
        std::vector<jfloat> buf(n * kStride);
        for (int i = 0; i < n; ++i) {
            const auto &f = af->featuresArray[i];
            buf[i * kStride + 0] = f.rms;
            buf[i * kStride + 1] = f.bass;
            buf[i * kStride + 2] = f.low_mid;
            buf[i * kStride + 3] = f.mid;
            buf[i * kStride + 4] = f.hi_mid;
            buf[i * kStride + 5] = f.treble;
        }
        env->SetFloatArrayRegion(jArr, 0, n * kStride, buf.data());
    }
    jobject cb = env->NewLocalRef(_callbackRef);
    if (cb && jArr) {
        jstring jSrc = env->NewStringUTF(src);
        env->CallVoidMethod(cb, _onAudioFeatures, jSrc, jArr);
        env->DeleteLocalRef(jSrc);
        env->DeleteLocalRef(cb);
    }
    if (jArr) env->DeleteLocalRef(jArr);
    if (didAttach) _jvm->DetachCurrentThread();
}

//  SynthAudioObserver

bool SynthAudioObserver::initialize(const va::audio::AudioConfig &config, JavaVM *jvm, jweak ref,
                                    jmethodID onAudioFeatures) {
    _provider = std::make_unique<va::thirdparty::VizAnimProvider>();
    if (!_provider->initialize(config)) {
        spdlog::warn("[SynthAudioObserver] VizAnimProvider failed to initialize");
        _provider.reset();
        return false;
    }
    _windowBytes = static_cast<size_t>(va::thirdparty::VizAnimProvider::FFT_SIZE) *
                   config.getSampleSizeInBytes();
    _accum.clear();
    _accum.reserve(_windowBytes * 2);
    _jvm = jvm;
    _callbackRef = ref;
    _onAudioFeatures = onAudioFeatures;
    return true;
}

void SynthAudioObserver::onPlayback(const char *data, size_t bytes) {
    if (!_provider || _windowBytes == 0) return;
    _accum.insert(_accum.end(), data, data + bytes);
    // Drain all complete windows, accumulate features, then fire a single JNI call.
    std::vector<va::thirdparty::VizAnimProvider::Features> batch;
    while (_accum.size() >= _windowBytes) {
        auto features = _provider->computeFeatures(_accum.data(),
                                                   static_cast<int>(va::thirdparty::VizAnimProvider::FFT_SIZE));
        for (auto &f: features) batch.push_back(std::move(f));
        _accum.erase(_accum.begin(),
                     _accum.begin() + static_cast<std::ptrdiff_t>(_windowBytes));
    }
    if (!batch.empty()) fireAudioFeatures(batch);
}

void SynthAudioObserver::fireAudioFeatures(const FeatureVec &batch) {
    if (!_jvm || !_callbackRef || !_onAudioFeatures || batch.empty()) return;
    JNIEnv *env = nullptr;
    bool didAttach = false;
    const jint rc = _jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, "vasdk-synth-anim", nullptr};
        if (_jvm->AttachCurrentThread(&env, &args) != JNI_OK || !env) return;
        didAttach = true;
    } else if (rc != JNI_OK || !env) return;

    static constexpr int kStride = 6;
    const int n = static_cast<int>(batch.size());
    jfloatArray jArr = env->NewFloatArray(n * kStride);
    if (jArr) {
        std::vector<jfloat> buf(n * kStride);
        for (int i = 0; i < n; ++i) {
            const auto &f = batch[i];
            buf[i * kStride + 0] = f.rms;
            buf[i * kStride + 1] = f.bass;
            buf[i * kStride + 2] = f.low_mid;
            buf[i * kStride + 3] = f.mid;
            buf[i * kStride + 4] = f.hi_mid;
            buf[i * kStride + 5] = f.treble;
        }
        env->SetFloatArrayRegion(jArr, 0, n * kStride, buf.data());
    }
    jobject cb = env->NewLocalRef(_callbackRef);
    if (cb && jArr) {
        jstring jSrc = env->NewStringUTF("SYNTH");
        env->CallVoidMethod(cb, _onAudioFeatures, jSrc, jArr);
        env->DeleteLocalRef(jSrc);
        env->DeleteLocalRef(cb);
    }
    if (jArr) env->DeleteLocalRef(jArr);
    if (didAttach) _jvm->DetachCurrentThread();
}

#endif // VASDK_HAS_VIZANIM
