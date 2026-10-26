#pragma once
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief NeedleEngine
 * Process-wide access to Cactus's needle engine with the Whistle speech model loaded.
 *
 * The engine keeps one global, non-thread-safe model per kind, so every provider instance
 * shares this singleton and every call is serialised on its mutex. On desktop the engine
 * is a shared library resolved at runtime; with VASDK_WHISTLE_STATIC_ENGINE (Android) it
 * is linked in. Once loaded, the engine and model stay resident for the process lifetime
 * (the engine has no unload call).
 */
class NeedleEngine
{
public:
    static NeedleEngine& instance();

    /**
     * Loads the engine (desktop: from enginePath, or the default library name next to the
     * model, then the system search path) and the .cact archive. Idempotent; a later call
     * with a different modelPath is ignored with a warning, since the engine holds one
     * speech model per process.
     */
    bool load(const std::string& modelPath, const std::string& enginePath);
    bool isLoaded() const;

    /**
     * Runs needle_transcribe and returns its JSON output. language / keywords may be empty
     * (detect the language, no biasing). Returns false and fills error on failure.
     */
    bool transcribe(const std::vector<float>& pcm, const std::string& language,
                    const std::string& keywords, bool wordTimestamps,
                    std::string& json, std::string& error);

    /** File name of the shared engine library on this platform (empty when statically linked). */
    static const char* defaultLibraryName();

private:
    NeedleEngine() = default;
    NeedleEngine(const NeedleEngine&) = delete;
    NeedleEngine& operator=(const NeedleEngine&) = delete;

    bool bindSymbols(const std::string& modelDir, const std::string& enginePath);

    using LoadFn       = int (*)(const unsigned char*, unsigned long long);
    using ModelsFn     = int (*)();
    using LastErrorFn  = const char* (*)();
    using TranscribeFn = int (*)(const float*, int, const char*, const char*, int, char*, int);

    mutable std::mutex _mtx;
    void* _library = nullptr;
    LoadFn       _load       = nullptr;
    ModelsFn     _models     = nullptr;
    LastErrorFn  _lastError  = nullptr;
    TranscribeFn _transcribe = nullptr;

    // The engine reads the archive in place, so the bytes must outlive every call.
    std::vector<unsigned char> _archive;
    std::string _modelPath;
    std::vector<char> _out;
};
