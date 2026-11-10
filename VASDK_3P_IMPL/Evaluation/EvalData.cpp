#include "EvalData.h"

#include <VASDK/ModelStore.h>
#include <VASDK/WavFile.h>

#include <google/protobuf/text_format.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace va::thirdparty::evaluation {

namespace {

bool fail(std::string* error, const std::string& text) {
    if (error) *error = text;
    return false;
}

bool inTier(const vasdk::eval::Clip& clip, vasdk::eval::Tier tier) {
    return tier == vasdk::eval::FULL || clip.tier() == vasdk::eval::SMOKE;
}

} // namespace

bool EvalData::load(const std::string& folder, std::string* error) {
    const auto path = std::filesystem::path(folder) / "references.txtpb";
    std::ifstream in(path);
    if (!in) return fail(error, "no " + path.string());
    std::stringstream text;
    text << in.rdbuf();
    if (!google::protobuf::TextFormat::ParseFromString(text.str(), &_set)) return fail(error, path.string() + " isn't a vasdk.eval.EvalSet");
    _folder = folder;
    return true;
}

std::vector<const vasdk::eval::Clip*> EvalData::clips(vasdk::eval::ClipKind kind, vasdk::eval::Tier tier, const std::string& language) const {
    std::vector<const vasdk::eval::Clip*> found;
    for (const auto& clip : _set.clips())
        if (clip.kind() == kind && inTier(clip, tier) && (language.empty() || clip.language() == language)) found.push_back(&clip);
    return found;
}

std::vector<std::string> EvalData::languages(vasdk::eval::ClipKind kind, vasdk::eval::Tier tier) const {
    std::vector<std::string> found;
    for (const auto* clip : clips(kind, tier))
        if (std::find(found.begin(), found.end(), clip->language()) == found.end()) found.push_back(clip->language());
    return found;
}

bool EvalData::audio(const vasdk::eval::Clip& clip, std::vector<short>& pcm, std::string* error) const {
    va::audio::WavAudio wav;
    const auto path = (std::filesystem::path(_folder) / clip.path()).string();
    if (!va::audio::readWav(path, wav, error)) return false;
    if (wav.sampleRate != kSampleRate || wav.channels != 1)
        return fail(error, path + ": " + std::to_string(wav.sampleRate) + " Hz, " + std::to_string(wav.channels) + " channels; the set is 16 kHz mono");
    pcm = va::audio::toPcm16(wav.samples.data(), wav.samples.size());
    return true;
}

std::string locateData(const std::string& data, const std::string& store, const std::string& manifest, std::string* error) {
    if (!data.empty()) {
        if (std::filesystem::exists(std::filesystem::path(data) / "references.txtpb")) return data;
        fail(error, "no references.txtpb in " + data);
        return {};
    }
    if (store.empty()) {
        fail(error, "no evaluation data: give --data <folder>, or --store <folder> to download it");
        return {};
    }
    std::ifstream in(manifest, std::ios::binary);
    if (!in) {
        fail(error, "no manifest " + manifest);
        return {};
    }
    std::stringstream text;
    text << in.rdbuf();
    va::ModelStoreOptions options;
    options.directory = store;
    options.manifest = text.str();
    va::ModelStore models(options);
    std::string reason;
    if (models.status(kSetModel) != va::ModelStatus::Present && !models.pull(kSetModel, {}, &reason)) {
        fail(error, "the evaluation data didn't download: " + reason);
        return {};
    }
    return (std::filesystem::path(store) / kSetFolder).string();
}

} // namespace va::thirdparty::evaluation
