// ModelStore: pulls from file:// sources in a temporary folder, so it runs offline. Hashing (FIPS
// 180-4 vectors), resuming, cancelling, a file that isn't the one named, the license gate, offline
// mode, per-machine files, paths that leave the store, configs, the built-in manifest and the C API.
// With --network: a real model from its real host (skipped offline).

#include <VASDK/ModelStore.h>
#include <VASDK/c/vasdk.h>
#include "vasdk/engine/engine_config.pb.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// SHA-256 test vectors (FIPS 180-4 examples)
const std::string kAbc = "abc";
const std::string kAbcSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
const std::string kTwoBlocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
const std::string kTwoBlocksSha = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
const std::string kMillionSha = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";   // 1,000,000 x 'a'

void write(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string fileUrl(const fs::path& path) {
    std::string generic = fs::absolute(path).generic_u8string();
    return generic.front() == '/' ? "file://" + generic : "file:///" + generic;
}

nlohmann::json fileEntry(const std::string& path, const fs::path& source, const std::string& sha256, std::size_t size,
                         const std::string& architecture = "") {
    nlohmann::json entry = {{"path", path}, {"url", fileUrl(source)}, {"sha256", sha256}, {"size", size}};
    if (!architecture.empty()) entry["arch"] = architecture;
    return entry;
}

/// --network: the built-in manifest's Silero VAD from its real host, checked against its pinned
/// SHA-256. Skipped (77) when the host can't be reached; a file that doesn't match fails.
int pullFromNetwork() {
    const fs::path store = fs::temp_directory_path() / "vasdk_model_store_network";
    fs::remove_all(store);
    const va::ModelStore models(va::ModelStoreOptions{store.u8string()});
    std::string error;
    if (!models.pull("silero_vad", {}, &error)) {
        std::printf("%s\n", error.c_str());
        fs::remove_all(store);
        if (error.find("SHA-256") != std::string::npos) return 1;
        std::printf("skipped: no network\n");
        return 77;
    }
    const bool verified = models.verify("silero_vad", &error) && fs::exists(store / "silero_vad.onnx");
    std::printf("%s\n", verified ? "silero_vad pulled and verified" : error.c_str());
    fs::remove_all(store);
    return verified ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--network") return pullFromNetwork();

    const fs::path root = fs::temp_directory_path() / "vasdk_model_store_test";
    fs::remove_all(root);
    const fs::path sources = root / "sources";
    const fs::path store = root / "store";
    write(sources / "abc.txt", kAbc);
    write(sources / "two.txt", kTwoBlocks);
    const std::string million(1000000, 'a');
    write(sources / "million.bin", million);

    nlohmann::json manifest = {{"schema", 1}, {"models", nlohmann::json::array()}};
    manifest["models"].push_back({{"id", "small"}, {"providers", {"vad/test"}}, {"license", "MIT"},
                                  {"files", {fileEntry("small/abc.txt", sources / "abc.txt", kAbcSha, 3),
                                             fileEntry("small/two.txt", sources / "two.txt", kTwoBlocksSha, kTwoBlocks.size())}}});
    manifest["models"].push_back({{"id", "big"}, {"providers", {"tts/test", "tts/other"}}, {"license", "MIT"},
                                  {"files", {fileEntry("big.bin", sources / "million.bin", kMillionSha, million.size())}}});
    manifest["models"].push_back({{"id", "wrong"}, {"providers", nlohmann::json::array()}, {"license", "MIT"},
                                  {"files", {fileEntry("wrong.txt", sources / "abc.txt", kTwoBlocksSha, 3)}}});
    manifest["models"].push_back({{"id", "terms"}, {"providers", {"llm/test"}}, {"license", "CC-BY-NC-4.0"},
                                  {"license_url", "https://example.com/license"}, {"requires_acceptance", true},
                                  {"files", {fileEntry("terms.txt", sources / "abc.txt", kAbcSha, 3)}}});
    manifest["models"].push_back({{"id", "per_machine"}, {"providers", nlohmann::json::array()}, {"license", "MIT"},
                                  {"files", {fileEntry("machine/model.txt", sources / "abc.txt", kAbcSha, 3, "arm64"),
                                             fileEntry("machine/model.txt", sources / "two.txt", kTwoBlocksSha, kTwoBlocks.size(), "x86_64"),
                                             fileEntry("machine/shared.txt", sources / "abc.txt", kAbcSha, 3)}}});

    va::ModelStoreOptions options;
    options.directory = store.u8string();
    options.manifest = manifest.dump();
    options.architecture = "x86_64";
    const va::ModelStore models(options);
    check(models.valid(), "manifest parses: " + models.error());
    check(models.models().size() == 5, "five models");

    // Pull, status, verify, the file where a provider looks
    check(models.status("small") == va::ModelStatus::Missing, "small missing at first");
    std::string error;
    std::vector<std::uint64_t> reported;
    va::ModelPullOptions pullOptions;
    pullOptions.progress = [&](const va::ModelProgress& progress) {
        reported.push_back(progress.done);
        return true;
    };
    check(models.pull("small", pullOptions, &error), "small pulls: " + error);
    check(models.status("small") == va::ModelStatus::Present, "small present");
    check(read(store / "small" / "abc.txt") == kAbc && read(store / "small" / "two.txt") == kTwoBlocks, "files in place");
    check(models.verify("small", &error), "small verifies: " + error);
    check(!reported.empty() && reported.back() == 3 + kTwoBlocks.size(), "progress reaches the model's size");
    check(models.pull("small", {}, &error), "pulling a present model is a no-op");

    // A multi-block file (1,000,000 bytes) hashed across resumes: half of it already downloaded
    write(store / "big.bin.part", million.substr(0, 400000));
    check(models.status("big") == va::ModelStatus::Partial, "a .part makes it partial");
    check(models.pull("big", {}, &error), "big resumes: " + error);
    check(models.verify("big", &error) && !fs::exists(store / "big.bin.part"), "resumed file verifies, .part gone");

    // Cancelled half way: partial, then the next pull finishes it
    check(models.remove("big", &error) && models.status("big") == va::ModelStatus::Missing, "remove");
    va::ModelPullOptions cancelling;
    cancelling.progress = [](const va::ModelProgress& progress) { return progress.done < 500000; };
    check(!models.pull("big", cancelling, &error) && error.find("cancelled") != std::string::npos, "cancel stops the pull: " + error);
    check(models.status("big") == va::ModelStatus::Partial, "cancelled pull is partial");
    check(models.pull("big", {}, &error) && models.verify("big", &error), "the next pull finishes it: " + error);

    // A file that isn't the one the manifest names: deleted, never in place
    check(!models.pull("wrong", {}, &error) && error.find("SHA-256") != std::string::npos, "hash mismatch fails: " + error);
    check(!fs::exists(store / "wrong.txt") && !fs::exists(store / "wrong.txt.part"), "mismatched file deleted");

    // A license to read first
    check(!models.pull("terms", {}, &error) && error.find("CC-BY-NC-4.0") != std::string::npos, "license gate: " + error);
    va::ModelPullOptions accepting;
    accepting.acceptLicense = true;
    check(models.pull("terms", accepting, &error), "pulls once accepted: " + error);

    // This machine's files only
    const va::ModelInfo* perMachine = models.find("per_machine");
    check(perMachine && perMachine->files.size() == 2 && perMachine->size() == kTwoBlocks.size() + 3, "x86_64 files only");
    check(models.pull("per_machine", {}, &error) && read(store / "machine" / "model.txt") == kTwoBlocks, "the x86_64 file: " + error);

    // Offline: what is present is fine, what is missing fails
    va::ModelStoreOptions offlineOptions = options;
    offlineOptions.offline = true;
    const va::ModelStore offline(offlineOptions);
    check(offline.pull("small", {}, &error), "offline: present model ok");
    check(offline.remove("big", &error) && !offline.pull("big", {}, &error) && error.find("offline") != std::string::npos,
          "offline: missing model fails: " + error);

    check(!models.pull("nothing", {}, &error) && error.find("no model") != std::string::npos, "unknown id");

    // Paths that would leave the store, a bad manifest
    va::ModelStoreOptions escaping = options;
    nlohmann::json bad = manifest;
    bad["models"][0]["files"][0]["path"] = "../outside.txt";
    escaping.manifest = bad.dump();
    check(!va::ModelStore(escaping).valid(), "a path outside the store is refused");
    escaping.manifest = "{not json";
    const va::ModelStore broken(escaping);
    check(!broken.valid() && broken.models().empty(), "a manifest that doesn't parse: no models");

    // What a config's providers load: its id and its fallbacks
    vasdk::engine::EngineConfig config;
    config.mutable_providers()->mutable_vad()->set_id("test");
    config.mutable_providers()->mutable_tts()->set_id("nothing");
    config.mutable_providers()->mutable_tts()->add_fallback("other");
    config.mutable_providers()->mutable_stt()->set_id("deepgram");
    const auto needed = models.modelsFor(config);
    check(needed == std::vector<std::string>({"small", "big"}), "modelsFor: vad and the tts fallback");

    // The built-in manifest: every file pinned
    const va::ModelStore builtin(va::ModelStoreOptions{store.u8string()});
    check(builtin.valid(), "built-in manifest parses: " + builtin.error());
    check(builtin.find("silero_vad") && builtin.find("whistle") && builtin.find("pocket_tts"), "built-in has the default flavor's models");
    for (const auto& model : builtin.models()) {
        check(!model.files.empty() && !model.license.empty(), model.id + ": files and a license");
        for (const auto& file : model.files)
            check(file.sha256.size() == 64 && file.size > 0 && file.url.rfind("https://", 0) == 0, model.id + "/" + file.path + " pinned");
    }
    check(builtin.modelsForProvider("vad/silero") == std::vector<std::string>({"silero_vad"}), "silero's model");
    check(builtin.modelsForProvider("entity_model/gliner") == std::vector<std::string>({"gliner"}), "a provider's first model is its default");

    // The C API over it
    check(vasdk_c_api_version() == VASDK_C_API_VERSION, "vasdk_c_api_version");
    write(root / "manifest.json", manifest.dump());
    const nlohmann::json cOptions = {{"directory", (root / "c_store").u8string()},
                                     {"manifest_path", (root / "manifest.json").u8string()},
                                     {"architecture", "x86_64"}};
    char* cError = nullptr;
    vasdk_models* cModels = vasdk_models_create(cOptions.dump().c_str(), &cError);
    check(cModels != nullptr, "vasdk_models_create");
    if (cModels) {
        check(std::string(vasdk_models_directory(cModels)) == (root / "c_store").u8string(), "vasdk_models_directory");
        check(vasdk_models_status(cModels, "small") == VASDK_MODEL_MISSING, "C: missing");
        check(vasdk_models_status(cModels, "nothing") == VASDK_MODEL_UNKNOWN, "C: unknown");
        int calls = 0;
        const int pulled = vasdk_models_pull(cModels, "small", 0,
            [](const char*, const char*, unsigned long long, unsigned long long, void* user) {
                ++*static_cast<int*>(user);
                return 1;
            }, &calls, &cError);
        check(pulled == 1 && calls > 0 && vasdk_models_status(cModels, "small") == VASDK_MODEL_PRESENT, "C: pull with progress");
        check(vasdk_models_verify(cModels, "small", &cError) == 1, "C: verify");
        check(vasdk_models_pull(cModels, "terms", 0, nullptr, nullptr, &cError) == 0 && cError, "C: license gate");
        vasdk_free(cError);
        cError = nullptr;
        char* list = vasdk_models_list(cModels);
        const auto listed = nlohmann::json::parse(list);
        check(listed.size() == 5 && listed[0]["id"] == "small" && listed[0]["status"] == "present", "C: list");
        vasdk_free(list);
        write(root / "engine.txtpb", "providers { vad { id: \"test\" } tts { id: \"test\" } }\n");
        char* forConfig = vasdk_models_for_config(cModels, (root / "engine.txtpb").u8string().c_str(), &cError);
        check(forConfig && nlohmann::json::parse(forConfig) == nlohmann::json({"small", "big"}), "C: for_config");
        vasdk_free(forConfig);
        check(vasdk_models_for_config(cModels, (root / "missing.txtpb").u8string().c_str(), &cError) == nullptr && cError,
              "C: for_config of a missing file");
        vasdk_free(cError);
        cError = nullptr;
        check(vasdk_models_remove(cModels, "small", &cError) == 1 && vasdk_models_status(cModels, "small") == VASDK_MODEL_MISSING,
              "C: remove");
        vasdk_models_free(cModels);
    }
    check(vasdk_models_create("{not json", &cError) == nullptr && cError, "C: bad options");
    vasdk_free(cError);
    char* defaultDirectory = vasdk_models_default_directory();
    check(defaultDirectory && std::string(defaultDirectory) == va::ModelStore::defaultDirectory(), "C: default directory");
    vasdk_free(defaultDirectory);

    fs::remove_all(root);
    if (failures == 0) std::printf("model store: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
