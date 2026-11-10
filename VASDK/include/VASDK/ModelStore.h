#pragma once

// The model store: downloads the model files providers load, checks them, and says where they
// are. One manifest lists every model (VASDK_3P_IMPL/models/manifest.json, built into the
// library): its files, where each comes from, its SHA-256 and its license. A file lands at
// <directory>/<path in the manifest>, the path a provider looks at under models_dir, so a session
// whose models_dir is the store's directory finds what the store pulled.
//
//   va::ModelStore store;                                     // VASDK_MODELS_DIR, else the user cache
//   for (const auto& id : store.modelsFor(config)) {          // what an engine config's providers load
//       std::string error;
//       if (!store.pull(id, {}, &error)) std::cerr << error << '\n';
//   }
//   // EngineConfig.models_dir unset: sessions look in store.directory()
//
// Downloads go to <file>.part and resume from there; a file is renamed into place once its size
// and SHA-256 match, so a present file is a whole one. A model whose license needs reading first
// (non-commercial, use restrictions, custom terms) is pulled only with acceptLicense. A gated
// Hugging Face model needs a token: options.token, else $HF_TOKEN, else the file `hf auth login`
// writes. docs/models.md.

#include <VASDK/vasdk_export.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vasdk::engine { class EngineConfig; }

namespace va {

struct ModelFile {
    std::string path;           // relative to the store's directory (and to models_dir)
    std::string url;
    std::string sha256;         // lowercase hex
    std::uint64_t size = 0;     // bytes
    std::string architecture;   // empty: every machine; else only this one ("x86_64", "arm64")
};

struct ModelInfo {
    std::string id;                          // "silero_vad"
    std::string description;
    std::vector<std::string> providers;      // "<stage>/<provider id>" that load it: "vad/silero"
    std::string license;                     // SPDX-style: "MIT", "CC-BY-NC-4.0"
    std::string licenseUrl;
    std::string revision;                    // the upstream commit or tag it is pinned to
    bool gated = false;                      // the host wants a token (accept the terms on its page first)
    bool requiresAcceptance = false;         // pulled only with ModelPullOptions::acceptLicense
    std::vector<ModelFile> files;            // this machine's files only
    std::uint64_t size() const;              // their total bytes
};

enum class ModelStatus {
    Missing,   // no file of it is in the store
    Partial,   // some files are, or a download stopped half way
    Present,   // every file is, with its size (verify() checks the hashes too)
};

struct ModelProgress {
    std::string model;
    std::string file;              // the file downloading now
    std::uint64_t done = 0;        // bytes of the model in the store so far
    std::uint64_t total = 0;       // the model's bytes
};

struct ModelStoreOptions {
    std::string directory;         // empty: ModelStore::defaultDirectory()
    std::string manifest;          // manifest JSON; empty: the one built into the library
    std::string token;             // Hugging Face token for gated models; empty: $HF_TOKEN, then `hf auth login`'s file
    bool offline = false;          // never download (also $VASDK_OFFLINE=1): pull() fails for what is missing
    std::string architecture;      // empty: this machine's ("x86_64", "arm64")
};

struct ModelPullOptions {
    bool acceptLicense = false;    // the caller read the license of a model that requiresAcceptance
    /// Called as bytes arrive; return false to stop (pull() then fails with "cancelled", and the
    /// next pull resumes)
    std::function<bool(const ModelProgress&)> progress;
};

class VASDK_EXPORT ModelStore {
public:
    explicit ModelStore(ModelStoreOptions options = {});

    /// $VASDK_MODELS_DIR, else the user's cache: %LOCALAPPDATA%\vasdk\models (Windows),
    /// ~/Library/Caches/vasdk/models (macOS), $XDG_CACHE_HOME/vasdk/models or ~/.cache/vasdk/models
    /// (Linux). Empty where none of these exists (Android: give the app's files directory).
    static std::string defaultDirectory();

    /// False when the manifest didn't parse: error() says why, and the store has no model
    bool valid() const { return _error.empty(); }
    const std::string& error() const { return _error; }
    const std::string& directory() const { return _options.directory; }

    const std::vector<ModelInfo>& models() const { return _models; }
    const ModelInfo* find(const std::string& id) const;
    /// The models of "<stage>/<provider id>" ("tts/pocket"), in manifest order
    std::vector<std::string> modelsForProvider(const std::string& provider) const;
    /// The models an engine config's providers load: each stage's provider and its fallbacks. A
    /// stage that names no provider (the first one built in) adds none: name it to have its model
    std::vector<std::string> modelsFor(const vasdk::engine::EngineConfig& config) const;

    ModelStatus status(const std::string& id) const;
    /// Downloads what is missing of the model. True when every file is present; false, with
    /// `error` set, for an unknown id, a license not accepted, offline, a network failure or a
    /// file whose SHA-256 doesn't match (it is deleted)
    bool pull(const std::string& id, const ModelPullOptions& options = {}, std::string* error = nullptr) const;
    /// Hashes every file of the model: true when all are present and match
    bool verify(const std::string& id, std::string* error = nullptr) const;
    /// Deletes the model's files (and partial downloads)
    bool remove(const std::string& id, std::string* error = nullptr) const;

private:
    ModelStoreOptions _options;
    std::vector<ModelInfo> _models;
    std::string _error;
};

/// The manifest built into the library (VASDK_3P_IMPL/models/manifest.json)
VASDK_EXPORT std::string builtinModelManifest();

} // namespace va
