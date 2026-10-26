#include "NeedleEngine.hpp"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <type_traits>

// needle.h marks its declarations with a GCC visibility attribute; only the prototypes are
// used here (to check the function-pointer types, or to link statically), so drop it.
#ifndef NEEDLE_API
#define NEEDLE_API
#endif
#include <needle.h>

#if !defined(VASDK_WHISTLE_STATIC_ENGINE)
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace {

constexpr size_t kInitialOutputBytes = 64 * 1024;
constexpr size_t kMaxOutputBytes     = 16 * 1024 * 1024;

#if !defined(VASDK_WHISTLE_STATIC_ENGINE)
void* openLibrary(const std::filesystem::path& path)
{
#ifdef _WIN32
    // LOAD_WITH_ALTERED_SEARCH_PATH resolves the engine's own imports from its folder.
    return reinterpret_cast<void*>(
        LoadLibraryExW(path.wstring().c_str(), nullptr,
                       path.has_parent_path() ? LOAD_WITH_ALTERED_SEARCH_PATH : 0));
#else
    return dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* findSymbol(void* library, const char* name)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

std::string lastLoaderError()
{
#ifdef _WIN32
    return "error " + std::to_string(GetLastError());
#else
    const char* e = dlerror();
    return e ? e : "unknown error";
#endif
}
#endif

} // namespace

NeedleEngine& NeedleEngine::instance()
{
    static NeedleEngine engine;
    return engine;
}

const char* NeedleEngine::defaultLibraryName()
{
#if defined(VASDK_WHISTLE_STATIC_ENGINE)
    return "";
#elif defined(_WIN32)
    return "libneedle3.dll";
#elif defined(__APPLE__)
    return "libneedle3.dylib";
#else
    return "libneedle3.so";
#endif
}

bool NeedleEngine::bindSymbols(const std::string& modelDir, const std::string& enginePath)
{
    static_assert(std::is_same<decltype(&needle_load), LoadFn>::value, "needle_load signature changed");
    static_assert(std::is_same<decltype(&needle_models), ModelsFn>::value, "needle_models signature changed");
    static_assert(std::is_same<decltype(&needle_last_error), LastErrorFn>::value, "needle_last_error signature changed");
    static_assert(std::is_same<decltype(&needle_transcribe), TranscribeFn>::value, "needle_transcribe signature changed");

#if defined(VASDK_WHISTLE_STATIC_ENGINE)
    (void)modelDir;
    (void)enginePath;
    _load       = &needle_load;
    _models     = &needle_models;
    _lastError  = &needle_last_error;
    _transcribe = &needle_transcribe;
    return true;
#else
    // Explicit path first, then the library next to the model, then the loader's search path.
    std::vector<std::filesystem::path> candidates;
    if (!enginePath.empty()) {
        candidates.emplace_back(enginePath);
    } else {
        candidates.push_back(std::filesystem::path(modelDir) / defaultLibraryName());
        candidates.emplace_back(defaultLibraryName());
    }

    for (const auto& path : candidates) {
        void* lib = openLibrary(path);
        if (!lib) {
            spdlog::debug("[WhistleSTT] Could not open '{}': {}", path.string(), lastLoaderError());
            continue;
        }
        auto load       = reinterpret_cast<LoadFn>(findSymbol(lib, "needle_load"));
        auto models     = reinterpret_cast<ModelsFn>(findSymbol(lib, "needle_models"));
        auto lastError  = reinterpret_cast<LastErrorFn>(findSymbol(lib, "needle_last_error"));
        auto transcribe = reinterpret_cast<TranscribeFn>(findSymbol(lib, "needle_transcribe"));
        if (!load || !models || !lastError || !transcribe) {
            spdlog::error("[WhistleSTT] '{}' is missing the needle speech API", path.string());
            continue; // deliberately not unloaded: closing a half-initialised engine is not worth the risk
        }
        _library    = lib;
        _load       = load;
        _models     = models;
        _lastError  = lastError;
        _transcribe = transcribe;
        spdlog::info("[WhistleSTT] Engine loaded from '{}'", path.string());
        return true;
    }

    spdlog::error("[WhistleSTT] Could not load the needle engine ({}). Re-run CMake with "
                  "VASDK_DOWNLOAD_WHISTLE_STT_MODEL=ON or pass the library path explicitly.",
                  defaultLibraryName());
    return false;
#endif
}

bool NeedleEngine::load(const std::string& modelPath, const std::string& enginePath)
{
    std::lock_guard<std::mutex> lock(_mtx);

    if (!_archive.empty()) {
        if (modelPath != _modelPath) {
            spdlog::warn("[WhistleSTT] Engine already holds '{}'; ignoring '{}' (one speech model per process)",
                         _modelPath, modelPath);
        }
        return true;
    }

    const std::string modelDir = std::filesystem::path(modelPath).parent_path().string();
    if (!_transcribe && !bindSymbols(modelDir, enginePath)) return false;

    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    if (!file) {
        spdlog::error("[WhistleSTT] Model file '{}' not found", modelPath);
        return false;
    }
    std::vector<unsigned char> archive(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(archive.data()), static_cast<std::streamsize>(archive.size()))) {
        spdlog::error("[WhistleSTT] Failed to read '{}'", modelPath);
        return false;
    }

    if (_load(archive.data(), archive.size()) < 0) {
        spdlog::error("[WhistleSTT] needle_load failed for '{}': {}", modelPath, _lastError());
        return false;
    }
    if ((_models() & NEEDLE_SPEECH) == 0) {
        spdlog::error("[WhistleSTT] '{}' does not contain a speech model", modelPath);
        return false;
    }

    _archive   = std::move(archive);
    _modelPath = modelPath;
    _out.assign(kInitialOutputBytes, '\0');
    return true;
}

bool NeedleEngine::isLoaded() const
{
    std::lock_guard<std::mutex> lock(_mtx);
    return !_archive.empty();
}

bool NeedleEngine::transcribe(const std::vector<float>& pcm, const std::string& language,
                              const std::string& keywords, bool wordTimestamps,
                              std::string& json, std::string& error)
{
    std::lock_guard<std::mutex> lock(_mtx);
    if (_archive.empty()) {
        error = "engine not loaded";
        return false;
    }

    const char* lang = language.empty() || language == "auto" ? nullptr : language.c_str();
    const char* kw   = keywords.empty() ? nullptr : keywords.c_str();

    // The API does not report the size it needs, so grow the buffer and retry when the
    // engine says it was too small ("output buffer is too small: ...").
    for (;;) {
        int rc = _transcribe(pcm.data(), static_cast<int>(pcm.size()), lang, kw, wordTimestamps ? 1 : 0,
                             _out.data(), static_cast<int>(_out.size()));
        if (rc >= 0) {
            json.assign(_out.data());
            return true;
        }
        const char* e = _lastError();
        error = e ? e : "needle_transcribe failed";
        if (error.find("too small") == std::string::npos || _out.size() >= kMaxOutputBytes) return false;
        _out.assign(_out.size() * 4, '\0');
    }
}
