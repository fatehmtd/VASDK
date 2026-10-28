#include "PyannoteEmbeddingProvider.h"
#include "KaldiFbank.h"
#include <onnxruntime_cxx_api.h>
#ifdef VASDK_USE_NNAPI
#include <nnapi_provider_factory.h>
#endif
#include <spdlog/spdlog.h>
#include <array>

namespace va {
namespace thirdparty {

struct PyannoteEmbeddingProvider::Impl {
    Ort::Env env{ ORT_LOGGING_LEVEL_WARNING, "PyannoteEmbedding" };
    std::unique_ptr<Ort::Session> session;
    std::string inputName, outputName;
    KaldiFbank fbank;
};

PyannoteEmbeddingProvider::PyannoteEmbeddingProvider(const std::string& modelPath) : _modelPath(modelPath)
{
}

PyannoteEmbeddingProvider::~PyannoteEmbeddingProvider() = default;

bool PyannoteEmbeddingProvider::initialize(const va::audio::AudioConfig& audioConfig)
{
    if (audioConfig.sampleRate != KaldiFbank::kSampleRate || audioConfig.channels != 1) {
        spdlog::error("[PyannoteEmbeddingProvider] Unsupported audio: {} Hz, {} channels. Requires 16 kHz mono.",
            audioConfig.sampleRate, audioConfig.channels);
        return false;
    }

    auto impl = std::make_unique<Impl>();
    Ort::SessionOptions sessionOptions;
    sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    sessionOptions.SetInterOpNumThreads(1);
    sessionOptions.SetIntraOpNumThreads(1);
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef VASDK_USE_NNAPI
    OrtSessionOptionsAppendExecutionProvider_Nnapi(sessionOptions, 0);
#endif
    try {
#ifdef _WIN32
        std::wstring wModelPath(_modelPath.begin(), _modelPath.end());
        impl->session = std::make_unique<Ort::Session>(impl->env, wModelPath.c_str(), sessionOptions);
#else
        impl->session = std::make_unique<Ort::Session>(impl->env, _modelPath.c_str(), sessionOptions);
#endif
    }
    catch (const Ort::Exception& e) {
        spdlog::error("[PyannoteEmbeddingProvider] Failed to load ONNX model '{}': {}", _modelPath, e.what());
        return false;
    }

    // Single input "feats" [N, T, 80] and single output "embs" [N, 256].
    Ort::AllocatorWithDefaultOptions allocator;
    impl->inputName = impl->session->GetInputNameAllocated(0, allocator).get();
    impl->outputName = impl->session->GetOutputNameAllocated(0, allocator).get();

    _impl = std::move(impl);
    _audioConfig = audioConfig;
    return true;
}

std::vector<float> PyannoteEmbeddingProvider::computeEmbedding(const float* samples, size_t numSamples)
{
    if (!_impl) {
        spdlog::error("[PyannoteEmbeddingProvider] computeEmbedding() called before initialize()");
        return {};
    }

    std::vector<float> scaled(samples, samples + numSamples);
    for (float& s : scaled) s *= 32768.0f;

    auto features = _impl->fbank.compute(scaled.data(), scaled.size());
    const int64_t numFrames = static_cast<int64_t>(features.size() / KaldiFbank::kNumBins);
    if (numFrames == 0) {
        return {};
    }

    // Cepstral mean normalization over the utterance, as in pyannote's WeSpeaker front end.
    for (int b = 0; b < KaldiFbank::kNumBins; ++b) {
        double mean = 0.0;
        for (int64_t t = 0; t < numFrames; ++t) mean += features[t * KaldiFbank::kNumBins + b];
        mean /= static_cast<double>(numFrames);
        for (int64_t t = 0; t < numFrames; ++t) features[t * KaldiFbank::kNumBins + b] -= static_cast<float>(mean);
    }

    try {
        const std::array<int64_t, 3> shape{ 1, numFrames, KaldiFbank::kNumBins };
        Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(memInfo, features.data(), features.size(),
            shape.data(), shape.size());
        const char* inputNames[] = { _impl->inputName.c_str() };
        const char* outputNames[] = { _impl->outputName.c_str() };
        auto outputs = _impl->session->Run(Ort::RunOptions{ nullptr }, inputNames, &input, 1, outputNames, 1);
        const float* data = outputs[0].GetTensorData<float>();
        const size_t size = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
        return std::vector<float>(data, data + size);
    }
    catch (const Ort::Exception& e) {
        spdlog::error("[PyannoteEmbeddingProvider] Inference failed: {}", e.what());
        return {};
    }
}

} // namespace thirdparty
} // namespace va
