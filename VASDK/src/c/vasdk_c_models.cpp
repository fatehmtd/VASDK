// The C API's model store (VASDK/c/vasdk.h, "Models") over va::ModelStore.
#include "c/vasdk.h"
#include "ModelStore.h"
#include "engine/ProtoFileLoader.h"
#include "vasdk/engine/engine_config.pb.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

struct vasdk_models {
    explicit vasdk_models(va::ModelStoreOptions options) : store(std::move(options)) {}
    va::ModelStore store;
};

namespace {

char* copyText(const std::string& text) {
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out) std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

template <class Result>
Result failWith(char** error, const std::string& message, Result result) {
    if (error) *error = copyText(message);
    return result;
}

const char* statusName(va::ModelStatus status) {
    switch (status) {
    case va::ModelStatus::Present: return "present";
    case va::ModelStatus::Partial: return "partial";
    case va::ModelStatus::Missing: break;
    }
    return "missing";
}

} // namespace

vasdk_models* vasdk_models_create(const char* options_json, char** error) {
    va::ModelStoreOptions options;
    if (options_json && *options_json) {
        const auto given = nlohmann::json::parse(options_json, nullptr, false);
        if (!given.is_object()) return failWith(error, "options are not a JSON object", static_cast<vasdk_models*>(nullptr));
        options.directory = given.value("directory", std::string());
        options.token = given.value("token", std::string());
        options.offline = given.value("offline", false);
        options.architecture = given.value("architecture", std::string());
        if (const auto manifestPath = given.value("manifest_path", std::string()); !manifestPath.empty()) {
            std::ifstream in(manifestPath, std::ios::binary);
            if (!in) return failWith(error, "can't read " + manifestPath, static_cast<vasdk_models*>(nullptr));
            std::ostringstream text;
            text << in.rdbuf();
            options.manifest = text.str();
        }
    }
    auto* models = new vasdk_models(std::move(options));
    if (!models->store.valid()) {
        const std::string reason = models->store.error();
        delete models;
        return failWith(error, reason, static_cast<vasdk_models*>(nullptr));
    }
    return models;
}

void vasdk_models_free(vasdk_models* models) { delete models; }

char* vasdk_models_default_directory(void) { return copyText(va::ModelStore::defaultDirectory()); }

const char* vasdk_models_directory(const vasdk_models* models) { return models ? models->store.directory().c_str() : ""; }

char* vasdk_models_list(const vasdk_models* models) {
    nlohmann::json list = nlohmann::json::array();
    if (models) {
        for (const auto& model : models->store.models()) {
            nlohmann::json files = nlohmann::json::array();
            for (const auto& file : model.files)
                files.push_back({{"path", file.path}, {"url", file.url}, {"sha256", file.sha256}, {"size", file.size}});
            list.push_back({{"id", model.id},
                            {"description", model.description},
                            {"providers", model.providers},
                            {"license", model.license},
                            {"license_url", model.licenseUrl},
                            {"revision", model.revision},
                            {"gated", model.gated},
                            {"requires_acceptance", model.requiresAcceptance},
                            {"size", model.size()},
                            {"status", statusName(models->store.status(model.id))},
                            {"files", files}});
        }
    }
    return copyText(list.dump());
}

char* vasdk_models_for_config(const vasdk_models* models, const char* config_path, char** error) {
    if (!models || !config_path) return failWith(error, "no model store or config path", static_cast<char*>(nullptr));
    vasdk::engine::EngineConfig config;
    std::string reason;
    if (!va::engine::loadEngineConfig(config_path, config, &reason)) return failWith(error, reason, static_cast<char*>(nullptr));
    return copyText(nlohmann::json(models->store.modelsFor(config)).dump());
}

vasdk_model_status vasdk_models_status(const vasdk_models* models, const char* id) {
    if (!models || !id || !models->store.find(id)) return VASDK_MODEL_UNKNOWN;
    switch (models->store.status(id)) {
    case va::ModelStatus::Present: return VASDK_MODEL_PRESENT;
    case va::ModelStatus::Partial: return VASDK_MODEL_PARTIAL;
    case va::ModelStatus::Missing: break;
    }
    return VASDK_MODEL_MISSING;
}

int vasdk_models_pull(const vasdk_models* models, const char* id, int accept_license, vasdk_model_progress_fn progress,
                      void* user, char** error) {
    if (!models || !id) return failWith(error, "no model store or id", 0);
    va::ModelPullOptions options;
    options.acceptLicense = accept_license != 0;
    if (progress)
        options.progress = [progress, user](const va::ModelProgress& at) {
            return progress(at.model.c_str(), at.file.c_str(), at.done, at.total, user) != 0;
        };
    std::string reason;
    if (!models->store.pull(id, options, &reason)) return failWith(error, reason, 0);
    return 1;
}

int vasdk_models_verify(const vasdk_models* models, const char* id, char** error) {
    if (!models || !id) return failWith(error, "no model store or id", 0);
    std::string reason;
    if (!models->store.verify(id, &reason)) return failWith(error, reason, 0);
    return 1;
}

int vasdk_models_remove(const vasdk_models* models, const char* id, char** error) {
    if (!models || !id) return failWith(error, "no model store or id", 0);
    std::string reason;
    if (!models->store.remove(id, &reason)) return failWith(error, reason, 0);
    return 1;
}
