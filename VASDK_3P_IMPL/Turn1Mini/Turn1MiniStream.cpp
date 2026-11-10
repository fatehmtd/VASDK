#include "Turn1MiniStream.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

#ifdef VASDK_USE_NNAPI
#include <nnapi_provider_factory.h>
#endif
#include <spdlog/spdlog.h>

namespace va::thirdparty::turn {

namespace {

constexpr const char* kAudioInput = "audio";
constexpr const char* kScoresOutput = "probs";

size_t elementCount(const std::vector<int64_t>& shape) {
    size_t count = 1;
    for (const int64_t dimension : shape) count *= static_cast<size_t>(dimension);
    return count;
}

} // namespace

Turn1MiniStream::Turn1MiniStream(std::string modelPath) : _modelPath(std::move(modelPath)) {}

Turn1MiniStream::~Turn1MiniStream() = default;

bool Turn1MiniStream::initialize() {
    _environment = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "Turn1Mini");
    Ort::SessionOptions sessionOptions;
    sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    sessionOptions.SetInterOpNumThreads(1);
    sessionOptions.SetIntraOpNumThreads(1);   // one thread is enough for a model this small
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef VASDK_USE_NNAPI
    OrtSessionOptionsAppendExecutionProvider_Nnapi(sessionOptions, 0);
#endif
    try {
#ifdef _WIN32
        const std::wstring widePath(_modelPath.begin(), _modelPath.end());
        _session = std::make_unique<Ort::Session>(_environment, widePath.c_str(), sessionOptions);
#else
        _session = std::make_unique<Ort::Session>(_environment, _modelPath.c_str(), sessionOptions);
#endif
    } catch (const Ort::Exception& error) {
        spdlog::error("[Turn1Mini] can't load {}: {}", _modelPath, error.what());
        return false;
    }

    // Bind by name: audio + state_<name> in, probs + new_state_<name> out
    _inputNameStorage = {kAudioInput};
    _outputNameStorage = {kScoresOutput};
    for (const char* state : kStateNames) {
        _inputNameStorage.push_back(std::string("state_") + state);
        _outputNameStorage.push_back(std::string("new_state_") + state);
    }
    Ort::AllocatorWithDefaultOptions allocator;
    std::vector<std::string> graphInputs, graphOutputs;
    std::vector<std::vector<int64_t>> graphInputShapes;
    for (size_t i = 0; i < _session->GetInputCount(); ++i) {
        graphInputs.emplace_back(_session->GetInputNameAllocated(i, allocator).get());
        graphInputShapes.push_back(_session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
    }
    for (size_t i = 0; i < _session->GetOutputCount(); ++i)
        graphOutputs.emplace_back(_session->GetOutputNameAllocated(i, allocator).get());

    _stateShapes.clear();
    for (size_t i = 0; i < _inputNameStorage.size(); ++i) {
        const auto found = std::find(graphInputs.begin(), graphInputs.end(), _inputNameStorage[i]);
        if (found == graphInputs.end()) {
            spdlog::error("[Turn1Mini] {} has no input '{}': not a turn-1-mini step graph", _modelPath, _inputNameStorage[i]);
            return false;
        }
        const auto& shape = graphInputShapes[static_cast<size_t>(found - graphInputs.begin())];
        if (std::any_of(shape.begin(), shape.end(), [](int64_t dimension) { return dimension < 1; })) {
            spdlog::error("[Turn1Mini] input '{}' has no fixed shape", _inputNameStorage[i]);
            return false;
        }
        if (i == 0) {
            if (elementCount(shape) != static_cast<size_t>(kStepSamples)) {
                spdlog::error("[Turn1Mini] input 'audio' takes {} samples, not {}", elementCount(shape), kStepSamples);
                return false;
            }
        } else {
            _stateShapes.push_back(shape);
        }
    }
    for (const auto& name : _outputNameStorage) {
        if (std::find(graphOutputs.begin(), graphOutputs.end(), name) == graphOutputs.end()) {
            spdlog::error("[Turn1Mini] {} has no output '{}': not a turn-1-mini step graph", _modelPath, name);
            return false;
        }
    }
    _inputNames.clear();
    _outputNames.clear();
    for (const auto& name : _inputNameStorage) _inputNames.push_back(name.c_str());
    for (const auto& name : _outputNameStorage) _outputNames.push_back(name.c_str());

    _memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    _stepAudio.assign(kStepSamples, 0.0f);
    reset();
    return true;
}

void Turn1MiniStream::reset() {
    if (!_session) return;
    _inputs.clear();
    const std::array<int64_t, 2> audioShape = {1, kStepSamples};
    _inputs.push_back(Ort::Value::CreateTensor<float>(_memoryInfo, _stepAudio.data(), _stepAudio.size(),
                                                      audioShape.data(), audioShape.size()));
    Ort::AllocatorWithDefaultOptions allocator;
    for (const auto& shape : _stateShapes) {
        auto state = Ort::Value::CreateTensor<float>(allocator, shape.data(), shape.size());
        std::fill_n(state.GetTensorMutableData<float>(), elementCount(shape), 0.0f);
        _inputs.push_back(std::move(state));
    }
    std::fill(_stepAudio.begin(), _stepAudio.end(), 0.0f);
    _pending = 0;
    _lastScore = 0.0f;
}

float Turn1MiniStream::runStep(bool keepState, std::vector<float>* scores) {
    auto outputs = _session->Run(Ort::RunOptions{nullptr}, _inputNames.data(), _inputs.data(), _inputs.size(),
                                 _outputNames.data(), _outputNames.size());
    const float* stepScores = outputs[0].GetTensorData<float>();
    const float now = stepScores[kScoresPerStep - 1];
    if (!keepState) return now;
    if (scores) scores->insert(scores->end(), stepScores, stepScores + kScoresPerStep);
    // The new state is the next step's state, unchanged
    for (size_t i = 1; i < outputs.size(); ++i) _inputs[i] = std::move(outputs[i]);
    _lastScore = now;
    return now;
}

int Turn1MiniStream::push(const float* samples, size_t count, std::vector<float>* scores) {
    if (!_session || !samples) return 0;
    int steps = 0;
    while (count > 0) {
        const size_t take = std::min(count, static_cast<size_t>(kStepSamples) - _pending);
        std::memcpy(_stepAudio.data() + _pending, samples, take * sizeof(float));
        _pending += take;
        samples += take;
        count -= take;
        if (_pending == static_cast<size_t>(kStepSamples)) {
            runStep(true, scores);
            _pending = 0;
            ++steps;
        }
    }
    return steps;
}

float Turn1MiniStream::scoreNow() {
    if (!_session || _pending == 0) return _lastScore;
    // Pad what has arrived with zeros and run on the current state, keeping that state for the full step
    std::fill(_stepAudio.begin() + static_cast<std::ptrdiff_t>(_pending), _stepAudio.end(), 0.0f);
    return runStep(false, nullptr);
}

} // namespace va::thirdparty::turn
