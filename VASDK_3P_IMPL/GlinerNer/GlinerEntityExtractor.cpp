#include "GlinerEntityExtractor.h"
#include <utility>

#include <nlohmann/json.hpp>
#include <onnxruntime_cxx_api.h>
#include <sentencepiece_processor.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

namespace va {
namespace thirdparty::nlu {

struct GlinerEntityExtractor::Model {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "GLiNER"};
    std::unique_ptr<Ort::Session> session;
    sentencepiece::SentencePieceProcessor tokenizer;
    int64_t cls = 1, end = 2, entityToken = 0, separatorToken = 0;
    int64_t maxWidth = 12;    // the longest span, in words
    size_t maxTypes = 25;     // labels it was trained to take at once
    size_t maxTokens = 384;
};

GlinerEntityExtractor::GlinerEntityExtractor(GlinerEntityExtractorOptions options)
    : _modelDir(std::move(options.modelDir)), _threshold(options.threshold), _threads(std::max(1, options.threads)) {}
GlinerEntityExtractor::~GlinerEntityExtractor() = default;

bool GlinerEntityExtractor::initialize() {
    auto model = std::make_unique<Model>();
    auto readJson = [this](const std::string& file, nlohmann::json& into) {
        std::ifstream in(_modelDir + "/" + file);
        if (!in) {
            spdlog::error("[GLiNER] no {}/{}", _modelDir, file);
            return false;
        }
        try {
            in >> into;
        } catch (const std::exception& error) {
            spdlog::error("[GLiNER] {}/{}: {}", _modelDir, file, error.what());
            return false;
        }
        return true;
    };
    nlohmann::json added, config;
    if (!readJson("added_tokens.json", added) || !readJson("gliner_config.json", config)) return false;
    model->entityToken = added.value(config.value("ent_token", std::string("<<ENT>>")), int64_t(0));
    model->separatorToken = added.value(config.value("sep_token", std::string("<<SEP>>")), int64_t(0));
    model->maxWidth = config.value("max_width", int64_t(12));
    model->maxTypes = config.value("max_types", size_t(25));
    model->maxTokens = config.value("max_len", size_t(384));
    if (model->entityToken == 0 || model->separatorToken == 0) {
        spdlog::error("[GLiNER] {}/added_tokens.json has no <<ENT>> or <<SEP>>", _modelDir);
        return false;
    }
    if (const auto status = model->tokenizer.Load(_modelDir + "/spm.model"); !status.ok()) {
        spdlog::error("[GLiNER] {}/spm.model: {}", _modelDir, status.ToString());
        return false;
    }
    model->cls = model->tokenizer.PieceToId("[CLS]");
    model->end = model->tokenizer.PieceToId("[SEP]");
    try {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(_threads);
        const std::string path = _modelDir + "/model.onnx";
#ifdef _WIN32
        const std::wstring widePath(path.begin(), path.end());
        model->session = std::make_unique<Ort::Session>(model->env, widePath.c_str(), options);
#else
        model->session = std::make_unique<Ort::Session>(model->env, path.c_str(), options);
#endif
    } catch (const Ort::Exception& error) {
        spdlog::error("[GLiNER] {}/model.onnx: {}", _modelDir, error.what());
        return false;
    }
    _model = std::move(model);
    spdlog::info("[GLiNER] loaded from {} (spans up to {} words)", _modelDir, _model->maxWidth);
    return true;
}

std::vector<va::nlu::EntitySpan> GlinerEntityExtractor::extract(const std::vector<std::string>& words,
                                                            const std::vector<va::nlu::EntityLabel>& labels, bool) const {
    if (!_model || words.empty() || labels.empty()) return {};
    const size_t types = std::min(labels.size(), _model->maxTypes);
    // [CLS] <<ENT>> label ... <<SEP>> word ... [SEP]; words_mask: each word's first piece, from 1.
    std::vector<int64_t> ids{_model->cls}, wordsMask{0};
    for (size_t type = 0; type < types; ++type) {
        std::vector<int> pieces;
        _model->tokenizer.Encode(labels[type].label, &pieces);
        ids.push_back(_model->entityToken);
        ids.insert(ids.end(), pieces.begin(), pieces.end());
        wordsMask.insert(wordsMask.end(), pieces.size() + 1, 0);
    }
    ids.push_back(_model->separatorToken);
    wordsMask.push_back(0);
    for (size_t word = 0; word < words.size(); ++word) {
        std::vector<int> pieces;
        _model->tokenizer.Encode(words[word], &pieces);
        if (pieces.empty()) pieces.push_back(_model->tokenizer.unk_id());
        ids.insert(ids.end(), pieces.begin(), pieces.end());
        wordsMask.push_back(static_cast<int64_t>(word + 1));
        wordsMask.insert(wordsMask.end(), pieces.size() - 1, 0);
    }
    ids.push_back(_model->end);
    wordsMask.push_back(0);
    if (ids.size() > _model->maxTokens) return {};

    const auto count = static_cast<int64_t>(words.size()), width = _model->maxWidth;
    std::vector<int64_t> attention(ids.size(), 1), lengths{count}, spanIndex;
    std::unique_ptr<bool[]> spanMask(new bool[static_cast<size_t>(count * width)]);
    for (int64_t start = 0; start < count; ++start)
        for (int64_t extra = 0; extra < width; ++extra) {
            spanIndex.push_back(start);
            spanIndex.push_back(start + extra);
            spanMask[static_cast<size_t>(start * width + extra)] = start + extra < count;
        }
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::array<int64_t, 2> tokenShape{1, static_cast<int64_t>(ids.size())}, lengthShape{1, 1}, maskShape{1, count * width};
    const std::array<int64_t, 3> spanShape{1, count * width, 2};
    std::vector<Ort::Value> inputs;
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, ids.data(), ids.size(), tokenShape.data(), 2));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, attention.data(), attention.size(), tokenShape.data(), 2));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, wordsMask.data(), wordsMask.size(), tokenShape.data(), 2));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, lengths.data(), 1, lengthShape.data(), 2));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(memory, spanIndex.data(), spanIndex.size(), spanShape.data(), 3));
    inputs.push_back(Ort::Value::CreateTensor<bool>(memory, spanMask.get(), static_cast<size_t>(count * width), maskShape.data(), 2));
    const std::array<const char*, 6> names{"input_ids", "attention_mask", "words_mask", "text_lengths", "span_idx", "span_mask"};
    const char* output = "logits";

    std::vector<va::nlu::EntitySpan> found;
    std::lock_guard<std::mutex> lock(_mutex);
    try {
        auto outputs = _model->session->Run(Ort::RunOptions{nullptr}, names.data(), inputs.data(), inputs.size(), &output, 1);
        const auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();   // [1, words, max_width, labels]
        if (shape.size() != 4 || shape[1] != count || shape[2] != width || static_cast<size_t>(shape[3]) != types) {
            spdlog::warn("[GLiNER] unexpected output shape");
            return {};
        }
        const float* logits = outputs[0].GetTensorData<float>();
        for (int64_t start = 0; start < count; ++start)
            for (int64_t extra = 0; extra < width && start + extra < count; ++extra)
                for (size_t type = 0; type < types; ++type) {
                    const float logit = logits[(static_cast<size_t>(start * width + extra)) * types + type];
                    const float score = 1.0f / (1.0f + std::exp(-logit));
                    if (score >= _threshold)
                        found.push_back({labels[type].entity, static_cast<size_t>(start), static_cast<size_t>(extra + 1), score});
                }
    } catch (const Ort::Exception& error) {
        spdlog::error("[GLiNER] extract: {}", error.what());
        return {};
    }
    return va::nlu::nonOverlapping(std::move(found));
}

} // namespace thirdparty::nlu
} // namespace va
