#include <VASDK/ModelStore.h>

#ifdef VASDK_HAS_ENGINE_CONFIG
#include "vasdk/engine/engine_config.pb.h"
#endif

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#ifdef VASDK_HAS_CURL
#include <curl/curl.h>
#endif

#include <algorithm>
#include <cctype>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace va {
namespace {

namespace fs = std::filesystem;

// ---- SHA-256 (FIPS 180-4), streamed --------------------------------------------------------

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

class Sha256 {
public:
    void update(const unsigned char* data, std::size_t length) {
        _length += length;
        while (length > 0) {
            const std::size_t take = (std::min)(length, _block.size() - _used);   // (): windows.h has a min macro
            std::memcpy(_block.data() + _used, data, take);
            _used += take;
            data += take;
            length -= take;
            if (_used == _block.size()) {
                compress(_block.data());
                _used = 0;
            }
        }
    }
    void update(const char* data, std::size_t length) { update(reinterpret_cast<const unsigned char*>(data), length); }

    std::string hex() {
        const std::uint64_t bits = _length * 8;
        const unsigned char one = 0x80;
        update(&one, 1);
        const unsigned char zero = 0;
        while (_used != 56) update(&zero, 1);
        std::array<unsigned char, 8> lengthBytes{};
        for (int index = 0; index < 8; ++index) lengthBytes[index] = static_cast<unsigned char>(bits >> (56 - 8 * index));
        update(lengthBytes.data(), lengthBytes.size());
        constexpr char digits[] = "0123456789abcdef";
        std::string out;
        for (const std::uint32_t word : _state)
            for (int shift = 28; shift >= 0; shift -= 4) out += digits[(word >> shift) & 0xf];
        return out;
    }

private:
    static std::uint32_t rotate(std::uint32_t value, int count) { return (value >> count) | (value << (32 - count)); }

    void compress(const unsigned char* block) {
        std::array<std::uint32_t, 64> schedule{};
        for (int index = 0; index < 16; ++index)
            schedule[index] = (std::uint32_t(block[4 * index]) << 24) | (std::uint32_t(block[4 * index + 1]) << 16) |
                              (std::uint32_t(block[4 * index + 2]) << 8) | std::uint32_t(block[4 * index + 3]);
        for (int index = 16; index < 64; ++index) {
            const std::uint32_t s0 = rotate(schedule[index - 15], 7) ^ rotate(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3);
            const std::uint32_t s1 = rotate(schedule[index - 2], 17) ^ rotate(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
            schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = _state;
        for (int index = 0; index < 64; ++index) {
            const std::uint32_t t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) +
                                     kRoundConstants[index] + schedule[index];
            const std::uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        const std::array<std::uint32_t, 8> added = {a, b, c, d, e, f, g, h};
        for (int index = 0; index < 8; ++index) _state[index] += added[index];
    }

    std::array<std::uint32_t, 8> _state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> _block{};
    std::size_t _used = 0;
    std::uint64_t _length = 0;
};

/// Hashes `path` into `hash`; false when it can't be read
bool hashFile(const fs::path& path, Sha256& hash) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hash.update(buffer.data(), static_cast<std::size_t>(in.gcount()));
    }
    return true;
}

// ---- Where things are ------------------------------------------------------------------------

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::string machineArchitecture() {
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    return "x86_64";
#else
    return "";
#endif
}

/// $HF_TOKEN, else the token `hf auth login` saved
std::string huggingFaceToken() {
    if (auto token = environment("HF_TOKEN"); !token.empty()) return token;
    std::vector<fs::path> candidates;
    if (auto home = environment("HF_HOME"); !home.empty()) candidates.push_back(fs::path(home) / "token");
    for (const char* home : {"USERPROFILE", "HOME"})
        if (auto value = environment(home); !value.empty()) candidates.push_back(fs::path(value) / ".cache" / "huggingface" / "token");
    for (const auto& candidate : candidates) {
        std::ifstream in(candidate);
        std::string token;
        if (in && std::getline(in, token)) {
            token.erase(token.find_last_not_of(" \r\n\t") + 1);
            if (!token.empty()) return token;
        }
    }
    return {};
}

bool startsWith(const std::string& text, const std::string& prefix) { return text.compare(0, prefix.size(), prefix) == 0; }

/// file:///C:/models/x.onnx or file:///models/x.onnx -> a path (%20 and the like decoded)
fs::path localPathOf(const std::string& url) {
    const std::string encoded = url.substr(std::strlen("file://"));
    std::string rest;
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '%' && index + 2 < encoded.size() && std::isxdigit(static_cast<unsigned char>(encoded[index + 1])) &&
            std::isxdigit(static_cast<unsigned char>(encoded[index + 2]))) {
            rest += static_cast<char>(std::stoi(encoded.substr(index + 1, 2), nullptr, 16));
            index += 2;
        } else {
            rest += encoded[index];
        }
    }
#ifdef _WIN32
    if (rest.size() > 2 && rest[0] == '/' && rest[2] == ':') rest.erase(0, 1);
#endif
    return fs::u8path(rest);
}

std::uint64_t sizeOr(const fs::path& path, std::uint64_t fallback) {
    std::error_code code;
    const auto size = fs::file_size(path, code);
    return code ? fallback : static_cast<std::uint64_t>(size);
}

// ---- One file's download ---------------------------------------------------------------------

struct Download {
    const ModelFile& file;
    fs::path partial;                    // <file>.part
    std::string token;                   // sent to huggingface.co only
    std::uint64_t modelDone = 0;         // bytes of the model's other files already in place
    ModelProgress progress;
    const std::function<bool(const ModelProgress&)>& report;

    std::ofstream out;
    Sha256 hash;
    std::uint64_t written = 0;           // bytes of this file in `partial`
    bool cancelled = false;

    bool accept(const char* data, std::size_t length) {
        out.write(data, static_cast<std::streamsize>(length));
        if (!out) return false;
        hash.update(data, length);
        written += length;
        progress.done = modelDone + written;
        if (report && !report(progress)) {
            cancelled = true;
            return false;
        }
        return true;
    }
};

bool fetchLocal(Download& download, std::string* error) {
    std::ifstream in(localPathOf(download.file.url), std::ios::binary);
    if (!in) {
        if (error) *error = "can't read " + download.file.url;
        return false;
    }
    in.seekg(static_cast<std::streamoff>(download.written));
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(in.gcount());
        if (count > 0 && !download.accept(buffer.data(), count)) {
            if (error) *error = download.cancelled ? "cancelled" : "can't write " + download.partial.string();
            return false;
        }
    }
    return true;
}

#ifdef VASDK_HAS_CURL
bool fetchRemote(Download& download, std::string* error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (error) *error = "curl_easy_init failed";
        return false;
    }
    struct Context {
        Download* download;
    } context{&download};

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, download.file.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);      // a stalled transfer: no byte in two minutes
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "VASDK-ModelStore");
    if (download.written > 0) curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(download.written));
    curl_slist* headers = nullptr;
    // Only to the Hub itself: curl drops it when a redirect leaves the host (its CDN)
    if (!download.token.empty() && startsWith(download.file.url, "https://huggingface.co/"))
        headers = curl_slist_append(headers, ("Authorization: Bearer " + download.token).c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* data, std::size_t size, std::size_t count, void* user) -> std::size_t {
        auto& download = *static_cast<Context*>(user)->download;
        return download.accept(data, size * count) ? size * count : 0;
    });

    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result == CURLE_OK) return true;
    if (result == CURLE_RANGE_ERROR && download.written > 0) {   // the server can't resume: start over
        download.out.close();
        download.out.open(download.partial, std::ios::binary | std::ios::trunc);
        download.hash = Sha256();
        download.written = 0;
        return fetchRemote(download, error);
    }
    if (!error) return false;
    if (download.cancelled) *error = "cancelled";
    else if (status == 401 || status == 403)
        *error = "HTTP " + std::to_string(status) + " for " + download.file.url +
                 ": a gated model; accept its terms on its page, then set HF_TOKEN (or run `hf auth login`)";
    else if (status > 0)
        *error = "HTTP " + std::to_string(status) + " for " + download.file.url;
    else
        *error = std::string(curl_easy_strerror(result)) + " for " + download.file.url;
    return false;
}
#endif

/// Downloads `file` into the store (resuming its .part), checks it and moves it into place
bool fetch(const ModelFile& file, const fs::path& target, Download& download, std::string* error) {
    std::error_code code;
    fs::create_directories(target.parent_path(), code);
    if (code) {
        if (error) *error = "can't create " + target.parent_path().string() + ": " + code.message();
        return false;
    }
    download.written = sizeOr(download.partial, 0);
    if (download.written > file.size) {   // longer than the file: not a part of it
        fs::remove(download.partial, code);
        download.written = 0;
    }
    if (download.written > 0 && !hashFile(download.partial, download.hash)) download.written = 0;

    if (download.written < file.size) {
        download.out.open(download.partial, std::ios::binary | (download.written > 0 ? std::ios::app : std::ios::trunc));
        if (!download.out) {
            if (error) *error = "can't write " + download.partial.string();
            return false;
        }
        bool fetched = false;
        if (startsWith(file.url, "file://")) {
            fetched = fetchLocal(download, error);
        } else {
#ifdef VASDK_HAS_CURL
            fetched = fetchRemote(download, error);
#else
            if (error) *error = "this VASDK was built without libcurl (VASDK_ENABLE_HTTP_TRANSPORT): can't download " + file.url;
#endif
        }
        download.out.close();
        if (!fetched) return false;
    }

    const std::string sha256 = download.hash.hex();
    if (download.written != file.size || sha256 != file.sha256) {
        fs::remove(download.partial, code);
        if (error)
            *error = file.path + " is not the file the manifest names (" + std::to_string(download.written) + " bytes, SHA-256 " +
                     sha256 + "; expected " + std::to_string(file.size) + " bytes, " + file.sha256 + "): deleted";
        return false;
    }
    fs::rename(download.partial, target, code);
    if (code) {
        if (error) *error = "can't move " + download.partial.string() + " into place: " + code.message();
        return false;
    }
    return true;
}

ModelInfo parseModel(const nlohmann::json& entry, const std::string& architecture) {
    ModelInfo model;
    model.id = entry.at("id").get<std::string>();
    model.description = entry.value("description", "");
    model.providers = entry.value("providers", std::vector<std::string>());
    model.license = entry.value("license", "");
    model.licenseUrl = entry.value("license_url", "");
    model.revision = entry.value("revision", "");
    model.gated = entry.value("gated", false);
    model.requiresAcceptance = entry.value("requires_acceptance", false);
    for (const auto& item : entry.at("files")) {
        ModelFile file;
        file.path = item.at("path").get<std::string>();
        file.url = item.at("url").get<std::string>();
        file.sha256 = item.at("sha256").get<std::string>();
        file.size = item.at("size").get<std::uint64_t>();
        file.architecture = item.value("arch", "");
        if (file.path.empty() || file.path.find("..") != std::string::npos || fs::u8path(file.path).is_absolute())
            throw std::runtime_error(model.id + ": file path '" + file.path + "' must stay inside the store");
        if (file.architecture.empty() || file.architecture == architecture) model.files.push_back(std::move(file));
    }
    return model;
}

} // namespace

std::uint64_t ModelInfo::size() const {
    std::uint64_t total = 0;
    for (const auto& file : files) total += file.size;
    return total;
}

std::string ModelStore::defaultDirectory() {
    if (auto directory = environment("VASDK_MODELS_DIR"); !directory.empty()) return directory;
#if defined(_WIN32)
    if (auto local = environment("LOCALAPPDATA"); !local.empty()) return (fs::u8path(local) / "vasdk" / "models").u8string();
#elif defined(__APPLE__)
    if (auto home = environment("HOME"); !home.empty()) return (fs::u8path(home) / "Library" / "Caches" / "vasdk" / "models").u8string();
#elif !defined(__ANDROID__)
    if (auto cache = environment("XDG_CACHE_HOME"); !cache.empty()) return (fs::u8path(cache) / "vasdk" / "models").u8string();
    if (auto home = environment("HOME"); !home.empty()) return (fs::u8path(home) / ".cache" / "vasdk" / "models").u8string();
#endif
    return {};
}

ModelStore::ModelStore(ModelStoreOptions options) : _options(std::move(options)) {
    if (_options.directory.empty()) _options.directory = defaultDirectory();
    if (_options.architecture.empty()) _options.architecture = machineArchitecture();
    if (_options.token.empty()) _options.token = huggingFaceToken();
    if (environment("VASDK_OFFLINE") == "1") _options.offline = true;
    const std::string text = _options.manifest.empty() ? builtinModelManifest() : _options.manifest;
    try {
        const auto manifest = nlohmann::json::parse(text);
        if (manifest.value("schema", 0) != 1) throw std::runtime_error("schema " + std::to_string(manifest.value("schema", 0)) + ", not 1");
        for (const auto& entry : manifest.at("models")) _models.push_back(parseModel(entry, _options.architecture));
    } catch (const std::exception& exception) {
        _models.clear();
        _error = std::string("model manifest: ") + exception.what();
    }
}

const ModelInfo* ModelStore::find(const std::string& id) const {
    const auto found = std::find_if(_models.begin(), _models.end(), [&](const ModelInfo& model) { return model.id == id; });
    return found == _models.end() ? nullptr : &*found;
}

std::vector<std::string> ModelStore::modelsForProvider(const std::string& provider) const {
    std::vector<std::string> ids;
    for (const auto& model : _models)
        if (std::find(model.providers.begin(), model.providers.end(), provider) != model.providers.end()) {
            ids.push_back(model.id);
            break;   // a provider's first model is its default; later ones are alternatives (gliner_multi)
        }
    return ids;
}

std::vector<std::string> ModelStore::modelsFor(const vasdk::engine::EngineConfig& config) const {
    std::vector<std::string> ids;
#ifdef VASDK_HAS_ENGINE_CONFIG
    const auto& providers = config.providers();
    const std::pair<const char*, const vasdk::engine::ProviderSpec*> stages[] = {
        {"vad", &providers.vad()},
        {"stt", &providers.stt()},
        {"tts", &providers.tts()},
        {"turn", &providers.turn()},
        {"wake_word", &providers.wake_word()},
        {"sentence_encoder", &providers.sentence_encoder()},
        {"entity_model", &providers.entity_model()},
        {"llm", &providers.llm()},
        {"audio_dsp", &providers.audio_dsp()},
        {"speaker_embedding", &providers.speaker_embedding()},
    };
    for (const auto& [stage, spec] : stages) {
        std::vector<std::string> names;
        if (!spec->id().empty()) names.push_back(spec->id());
        names.insert(names.end(), spec->fallback().begin(), spec->fallback().end());
        for (const auto& name : names)
            for (const auto& id : modelsForProvider(std::string(stage) + "/" + name))
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
#else
    (void)config;
#endif
    return ids;
}

ModelStatus ModelStore::status(const std::string& id) const {
    const ModelInfo* model = find(id);
    if (!model) return ModelStatus::Missing;
    std::size_t present = 0;
    bool partial = false;
    for (const auto& file : model->files) {
        const fs::path target = fs::u8path(_options.directory) / fs::u8path(file.path);
        if (sizeOr(target, file.size + 1) == file.size) ++present;
        else if (fs::exists(target.string() + ".part")) partial = true;
    }
    if (present == model->files.size()) return ModelStatus::Present;
    return present > 0 || partial ? ModelStatus::Partial : ModelStatus::Missing;
}

bool ModelStore::pull(const std::string& id, const ModelPullOptions& options, std::string* error) const {
    auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (!valid()) return fail(_error);
    const ModelInfo* model = find(id);
    if (!model) return fail("no model '" + id + "' in the manifest");
    if (_options.directory.empty()) return fail("the model store has no directory: give one (VASDK_MODELS_DIR)");
    if (model->requiresAcceptance && !options.acceptLicense)
        return fail(id + " is under " + model->license + ": read " + model->licenseUrl + ", then pull it accepting the license");

    ModelProgress progress{id, "", 0, model->size()};
    for (const auto& file : model->files) {
        const fs::path target = fs::u8path(_options.directory) / fs::u8path(file.path);
        if (sizeOr(target, file.size + 1) == file.size) {
            progress.done += file.size;
            continue;
        }
        if (_options.offline) return fail(id + ": " + file.path + " is missing and the store is offline");
        spdlog::info("[ModelStore] {}: downloading {} ({} MB)", id, file.path, file.size / 1000000);
        progress.file = file.path;
        Download download{file, fs::path(target.string() + ".part"), _options.token, progress.done, progress, options.progress};
        std::string reason;
        if (!fetch(file, target, download, &reason)) return fail(id + ": " + reason);
        progress.done += file.size;
    }
    if (options.progress) {
        progress.file.clear();
        options.progress(progress);
    }
    return true;
}

bool ModelStore::verify(const std::string& id, std::string* error) const {
    const ModelInfo* model = find(id);
    if (!model) {
        if (error) *error = "no model '" + id + "' in the manifest";
        return false;
    }
    for (const auto& file : model->files) {
        const fs::path target = fs::u8path(_options.directory) / fs::u8path(file.path);
        Sha256 hash;
        if (!hashFile(target, hash)) {
            if (error) *error = id + ": " + file.path + " is missing";
            return false;
        }
        if (const auto sha256 = hash.hex(); sha256 != file.sha256) {
            if (error) *error = id + ": " + file.path + " has SHA-256 " + sha256 + ", expected " + file.sha256;
            return false;
        }
    }
    return true;
}

bool ModelStore::remove(const std::string& id, std::string* error) const {
    const ModelInfo* model = find(id);
    if (!model) {
        if (error) *error = "no model '" + id + "' in the manifest";
        return false;
    }
    for (const auto& file : model->files) {
        const fs::path target = fs::u8path(_options.directory) / fs::u8path(file.path);
        std::error_code code;
        fs::remove(target, code);
        fs::remove(target.string() + ".part", code);
        if (code) {
            if (error) *error = "can't delete " + target.string() + ": " + code.message();
            return false;
        }
    }
    return true;
}

} // namespace va
