#include "MiniLmSentenceEncoder.h"
#include <utility>

#include <onnxruntime_cxx_api.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cctype>
#include <cmath>
#include <fstream>

namespace va {
namespace thirdparty::nlu {

struct MiniLmSentenceEncoder::Model {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "MiniLM"};
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputNames;   // input_ids, attention_mask, token_type_ids, in the model's order
    std::string outputName;
};

MiniLmSentenceEncoder::MiniLmSentenceEncoder(MiniLmSentenceEncoderOptions options)
    : _modelDir(std::move(options.modelDir)) {}
MiniLmSentenceEncoder::~MiniLmSentenceEncoder() = default;

bool MiniLmSentenceEncoder::initialize() {
    std::ifstream vocabulary(_modelDir + "/vocab.txt");
    if (!vocabulary) {
        spdlog::error("[MiniLM] no vocabulary at {}/vocab.txt", _modelDir);
        return false;
    }
    std::string token;
    for (int64_t id = 0; std::getline(vocabulary, token); ++id) {
        if (!token.empty() && token.back() == '\r') token.pop_back();
        _vocabulary.emplace(token, id);
    }
    try {
        _model = std::make_unique<Model>();
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        const std::string path = _modelDir + "/model.onnx";
#ifdef _WIN32
        const std::wstring widePath(path.begin(), path.end());
        _model->session = std::make_unique<Ort::Session>(_model->env, widePath.c_str(), options);
#else
        _model->session = std::make_unique<Ort::Session>(_model->env, path.c_str(), options);
#endif
        Ort::AllocatorWithDefaultOptions allocator;
        for (size_t i = 0; i < _model->session->GetInputCount(); ++i)
            _model->inputNames.push_back(_model->session->GetInputNameAllocated(i, allocator).get());
        _model->outputName = _model->session->GetOutputNameAllocated(0, allocator).get();
    } catch (const Ort::Exception& error) {
        spdlog::error("[MiniLM] {}/model.onnx: {}", _modelDir, error.what());
        _model.reset();
        return false;
    }
    spdlog::info("[MiniLM] loaded from {} ({} tokens)", _modelDir, _vocabulary.size());
    return true;
}

std::vector<int64_t> MiniLmSentenceEncoder::pieces(const std::string& word) const {
    auto id = [this](const std::string& piece) {
        const auto found = _vocabulary.find(piece);
        return found == _vocabulary.end() ? int64_t(-1) : found->second;
    };
    std::string text;
    for (const char c : word) text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // WordPiece: the longest piece in the vocabulary from the start, then "##" pieces for the rest.
    std::vector<int64_t> found;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.size();
        int64_t piece = -1;
        for (; end > start; --end) {
            piece = id((start > 0 ? "##" : "") + text.substr(start, end - start));
            if (piece >= 0) break;
        }
        if (piece < 0) return {id("[UNK]")};
        found.push_back(piece);
        start = end;
    }
    return found;
}

std::vector<int64_t> MiniLmSentenceEncoder::tokenize(const std::string& sentence, size_t maxTokens) const {
    auto id = [this](const std::string& piece) {
        const auto found = _vocabulary.find(piece);
        return found == _vocabulary.end() ? int64_t(-1) : found->second;
    };
    std::vector<int64_t> ids{id("[CLS]")};
    // Basic tokenizing: lower case, words split on spaces and punctuation (each punctuation its own word).
    std::vector<std::string> words;
    std::string word;
    for (const char c : sentence) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isspace(byte)) {
            if (!word.empty()) words.push_back(std::move(word));
            word.clear();
        } else if (std::ispunct(byte)) {
            if (!word.empty()) words.push_back(std::move(word));
            word.clear();
            words.emplace_back(1, c);
        } else {
            word += static_cast<char>(std::tolower(byte));
        }
    }
    if (!word.empty()) words.push_back(std::move(word));
    for (const auto& text : words)
        for (const auto piece : pieces(text))
            if (ids.size() + 1 < maxTokens) ids.push_back(piece);
    ids.push_back(id("[SEP]"));
    return ids;
}

std::vector<float> MiniLmSentenceEncoder::hidden(const std::vector<int64_t>& ids, size_t& size) const {
    if (!_model || !_model->session) return {};
    const std::vector<int64_t> mask(ids.size(), 1), types(ids.size(), 0);
    const std::array<int64_t, 2> shape{1, static_cast<int64_t>(ids.size())};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<Ort::Value> inputs;
    std::vector<const char*> names;
    for (const auto& name : _model->inputNames) {
        const auto& data = name.find("mask") != std::string::npos ? mask : name.find("type") != std::string::npos ? types : ids;
        inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, const_cast<int64_t*>(data.data()), data.size(), shape.data(),
                                                           shape.size()));
        names.push_back(name.c_str());
    }
    const char* output = _model->outputName.c_str();
    std::lock_guard<std::mutex> lock(_mutex);
    try {
        auto outputs = _model->session->Run(Ort::RunOptions{nullptr}, names.data(), inputs.data(), inputs.size(), &output, 1);
        const auto info = outputs[0].GetTensorTypeAndShapeInfo().GetShape();   // [1, tokens, 384]
        if (info.size() != 3) return {};
        const size_t tokens = static_cast<size_t>(info[1]);
        size = static_cast<size_t>(info[2]);
        const float* data = outputs[0].GetTensorData<float>();
        return std::vector<float>(data, data + tokens * size);
    } catch (const Ort::Exception& error) {
        spdlog::error("[MiniLM] encode: {}", error.what());
        return {};
    }
}

namespace {
// The mean of rows [from, to) of `states`, length 1.
std::vector<float> meanOf(const std::vector<float>& states, size_t size, size_t from, size_t to) {
    std::vector<float> mean(size, 0.0f);
    for (size_t t = from; t < to; ++t)
        for (size_t i = 0; i < size; ++i) mean[i] += states[t * size + i];
    float length = 0;
    for (auto& value : mean) {
        value /= static_cast<float>(to - from);
        length += value * value;
    }
    length = std::sqrt(length);
    if (length > 0)
        for (auto& value : mean) value /= length;
    return mean;
}
} // namespace

std::vector<float> MiniLmSentenceEncoder::encode(const std::string& sentence) const {
    const auto ids = tokenize(sentence);
    size_t size = 0;
    const auto states = hidden(ids, size);
    if (states.empty() || size == 0) return {};
    return meanOf(states, size, 0, states.size() / size);
}

std::vector<std::vector<float>> MiniLmSentenceEncoder::encodeWords(const std::vector<std::string>& words) const {
    auto id = [this](const std::string& piece) {
        const auto found = _vocabulary.find(piece);
        return found == _vocabulary.end() ? int64_t(-1) : found->second;
    };
    std::vector<int64_t> ids{id("[CLS]")};
    std::vector<std::pair<size_t, size_t>> spans;   // each word's pieces: [first, end)
    for (const auto& word : words) {
        const auto found = pieces(word);
        spans.emplace_back(ids.size(), ids.size() + found.size());
        ids.insert(ids.end(), found.begin(), found.end());
    }
    ids.push_back(id("[SEP]"));
    if (words.empty() || ids.size() > 128) return {};
    size_t size = 0;
    const auto states = hidden(ids, size);
    if (states.empty() || size == 0) return {};
    std::vector<std::vector<float>> vectors;
    for (const auto& [from, to] : spans) vectors.push_back(meanOf(states, size, from, to));
    return vectors;
}

} // namespace thirdparty::nlu
} // namespace va
