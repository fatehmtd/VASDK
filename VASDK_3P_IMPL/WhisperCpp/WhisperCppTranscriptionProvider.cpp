#include "WhisperCppTranscriptionProvider.hpp"
#include <whisper.h>
#include <spdlog/spdlog.h>
#include <cstring>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WhisperCppTranscriptionProvider::WhisperCppTranscriptionProvider(const std::string& modelPath)
    : _modelPath(modelPath)
{
}

WhisperCppTranscriptionProvider::~WhisperCppTranscriptionProvider()
{
    if (_running) stopTranscriptionProvider();
    releaseTranscriptionProvider();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool WhisperCppTranscriptionProvider::initializeTranscriptionProvider()
{
    if (_ctx) {
        spdlog::warn("[WhisperCpp] Already initialized");
        return true;
    }

    whisper_context_params cparams = whisper_context_default_params();
    _ctx = whisper_init_from_file_with_params(_modelPath.c_str(), cparams);
    if (!_ctx) {
        spdlog::error("[WhisperCpp] Failed to load model from '{}'", _modelPath);
        return false;
    }

    spdlog::info("[WhisperCpp] Initialized with model '{}'", _modelPath);
    return true;
}

void WhisperCppTranscriptionProvider::releaseTranscriptionProvider()
{
    if (_ctx) {
        whisper_free(_ctx);
        _ctx = nullptr;
    }
}

bool WhisperCppTranscriptionProvider::startTranscriptionProvider()
{
    if (_running) return true;
    if (!_ctx) {
        spdlog::error("[WhisperCpp] Not initialized, cannot start");
        return false;
    }
    if (_audioConfig.sampleRate != 16000) {
        spdlog::warn("[WhisperCpp] Audio sample rate is {} Hz; whisper.cpp requires 16000 Hz",
                     _audioConfig.sampleRate);
    }

    _running = true;
    _hasNewAudio = false;
    _lastWriteTime = std::chrono::steady_clock::now();
    _thread = std::thread(&WhisperCppTranscriptionProvider::inferenceLoop, this);

    spdlog::info("[WhisperCpp] Started");
    return true;
}

void WhisperCppTranscriptionProvider::stopTranscriptionProvider()
{
    if (!_running) return;

    _running = false;
    _cv.notify_all();
    if (_thread.joinable()) _thread.join();

    std::lock_guard<std::mutex> lock(_mtx);
    _buffer.clear();
    _hasNewAudio = false;
    _endOfSpeech = false;

    spdlog::info("[WhisperCpp] Stopped");
}

bool WhisperCppTranscriptionProvider::isTranscriptionProviderRunning() const
{
    return _running;
}

// ---------------------------------------------------------------------------
// Audio input
// ---------------------------------------------------------------------------

size_t WhisperCppTranscriptionProvider::writeAudioData(const uint8_t* data, size_t size)
{
    if (!_running || !data || size == 0) return 0;

    {
        std::lock_guard<std::mutex> lock(_mtx);
        appendAudioAsFloat(data, size);
        _lastWriteTime = std::chrono::steady_clock::now();
        _hasNewAudio = true;
    }
    _cv.notify_one();
    return size;
}

va::audio::TranscriptionResult WhisperCppTranscriptionProvider::writeFullAudioData(
    const uint8_t* data, size_t size)
{
    va::audio::TranscriptionResult result;
    if (!_ctx) {
        spdlog::error("[WhisperCpp] writeFullAudioData called before initialization");
        return result;
    }
    if (!data || size == 0) return result;

    // Convert raw bytes to float samples into a local buffer (does not touch _buffer).
    std::vector<float> audioFloat;
    switch (_audioConfig.format) {
    case va::audio::AudioFormat::SHORT: {
        size_t n = size / sizeof(int16_t);
        const auto* s = reinterpret_cast<const int16_t*>(data);
        audioFloat.resize(n);
        for (size_t i = 0; i < n; ++i)
            audioFloat[i] = static_cast<float>(s[i]) / 32768.0f;
        break;
    }
    case va::audio::AudioFormat::FLOAT: {
        size_t n = size / sizeof(float);
        const auto* s = reinterpret_cast<const float*>(data);
        audioFloat.assign(s, s + n);
        break;
    }
    case va::audio::AudioFormat::INT: {
        size_t n = size / sizeof(int32_t);
        const auto* s = reinterpret_cast<const int32_t*>(data);
        audioFloat.resize(n);
        for (size_t i = 0; i < n; ++i)
            audioFloat[i] = static_cast<float>(s[i]) / 2147483648.0f;
        break;
    }
    case va::audio::AudioFormat::BYTE: {
        audioFloat.resize(size);
        for (size_t i = 0; i < size; ++i)
            audioFloat[i] = (static_cast<float>(data[i]) - 128.0f) / 128.0f;
        break;
    }
    default:
        spdlog::warn("[WhisperCpp] writeFullAudioData: unknown format, treating as int16");
        size_t n = size / sizeof(int16_t);
        const auto* s = reinterpret_cast<const int16_t*>(data);
        audioFloat.resize(n);
        for (size_t i = 0; i < n; ++i)
            audioFloat[i] = static_cast<float>(s[i]) / 32768.0f;
        break;
    }

    std::lock_guard<std::mutex> lock(_inferenceMtx);
    auto ir          = runInference(audioFloat);
    result.text      = std::move(ir.text);
    result.confidence = ir.confidence;
    result.words     = std::move(ir.words);

    if (!result.words.empty()) {
        result.startTimeMs = result.words.front().startTimeMs;
        result.endTimeMs   = result.words.back().endTimeMs;
    }

    return result;
}

void WhisperCppTranscriptionProvider::commitEndOfSpeech()
{
    {
        std::lock_guard<std::mutex> lock(_mtx);
        if (!_buffer.empty() && _hasNewAudio) {
            _endOfSpeech = true;
            _bufferSizeAtEndOfSpeech = _buffer.size();
        }
    }
    _cv.notify_one();
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void WhisperCppTranscriptionProvider::setTranscriptionResultListener(
    va::audio::TranscriptionResultListener* listener)
{
    _listener = listener;
}

va::audio::TranscriptionResultListener*
WhisperCppTranscriptionProvider::getTranscriptionResultListener() const
{
    return _listener;
}

void WhisperCppTranscriptionProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig WhisperCppTranscriptionProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

void WhisperCppTranscriptionProvider::setInferenceConfig(const InferenceConfig& config)
{
    std::lock_guard<std::mutex> lock(_mtx);
    _inferenceConfig = config;
}

WhisperCppTranscriptionProvider::InferenceConfig
WhisperCppTranscriptionProvider::getInferenceConfig() const
{
    return _inferenceConfig;
}

// ---------------------------------------------------------------------------
// Audio format conversion (mirrors Parakeet implementation)
// ---------------------------------------------------------------------------

void WhisperCppTranscriptionProvider::appendAudioAsFloat(const uint8_t* data, size_t size)
{
    switch (_audioConfig.format) {
    case va::audio::AudioFormat::SHORT: {
        size_t numSamples = size / sizeof(int16_t);
        const auto* samples = reinterpret_cast<const int16_t*>(data);
        _buffer.reserve(_buffer.size() + numSamples);
        for (size_t i = 0; i < numSamples; ++i)
            _buffer.push_back(static_cast<float>(samples[i]) / 32768.0f);
        break;
    }
    case va::audio::AudioFormat::FLOAT: {
        size_t numSamples = size / sizeof(float);
        const auto* samples = reinterpret_cast<const float*>(data);
        _buffer.insert(_buffer.end(), samples, samples + numSamples);
        break;
    }
    case va::audio::AudioFormat::INT: {
        size_t numSamples = size / sizeof(int32_t);
        const auto* samples = reinterpret_cast<const int32_t*>(data);
        _buffer.reserve(_buffer.size() + numSamples);
        for (size_t i = 0; i < numSamples; ++i)
            _buffer.push_back(static_cast<float>(samples[i]) / 2147483648.0f);
        break;
    }
    case va::audio::AudioFormat::BYTE: {
        _buffer.reserve(_buffer.size() + size);
        for (size_t i = 0; i < size; ++i)
            _buffer.push_back((static_cast<float>(data[i]) - 128.0f) / 128.0f);
        break;
    }
    default:
        spdlog::warn("[WhisperCpp] Unknown audio format, treating as int16");
        size_t numSamples = size / sizeof(int16_t);
        const auto* samples = reinterpret_cast<const int16_t*>(data);
        _buffer.reserve(_buffer.size() + numSamples);
        for (size_t i = 0; i < numSamples; ++i)
            _buffer.push_back(static_cast<float>(samples[i]) / 32768.0f);
        break;
    }
}

// ---------------------------------------------------------------------------
// Inference
// ---------------------------------------------------------------------------

WhisperCppTranscriptionProvider::InferenceResult
WhisperCppTranscriptionProvider::runInference(const std::vector<float>& audio)
{
    InferenceResult result;
    if (!_ctx || audio.empty()) return result;

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads         = _inferenceConfig.threads;
    params.translate         = _inferenceConfig.translateToEnglish;
    params.print_progress    = false;
    params.print_realtime    = false;
    params.print_special     = false;
    params.no_timestamps     = false;
    params.token_timestamps  = true;  // enables word-level timing
    params.no_context        = _inferenceConfig.noContext;
    params.no_speech_thold   = _inferenceConfig.noSpeechThreshold;

    const std::string& lang = _inferenceConfig.language;
    if (lang == "auto" || lang.empty()) {
        // Pass "auto" as a non-null string; whisper.cpp's whisper_lang_id()
        // calls strcmp() on this pointer so nullptr causes UB / silent failure.
        params.language        = "auto";
        params.detect_language = true;
    } else {
        params.language        = lang.c_str();
        params.detect_language = false;
    }

    int ret = whisper_full(_ctx, params, audio.data(), static_cast<int>(audio.size()));
    if (ret != 0) {
        spdlog::error("[WhisperCpp] whisper_full failed (code {})", ret);
        return result;
    }

    int nSegments = whisper_full_n_segments(_ctx);
    if (nSegments == 0) {
        spdlog::debug("[WhisperCpp] No segments returned (no_speech suppression or empty audio)");
        return result;
    }
    std::string fullText;
    std::vector<va::audio::TranscriptionResult::Word> words;
    float totalProb   = 0.0f;
    int   tokenCount  = 0;

    for (int seg = 0; seg < nSegments; ++seg) {
        const char* segText = whisper_full_get_segment_text(_ctx, seg);
        if (segText) fullText += segText;

        // Aggregate sub-word tokens into word-level results.
        // In whisper.cpp, word-boundary tokens begin with a space character.
        // Special/timing tokens are enclosed in brackets, e.g. [_BEG_].
        va::audio::TranscriptionResult::Word currentWord;
        bool  inWord       = false;
        float wordProbSum  = 0.0f;
        int   wordTokCount = 0;

        int nTokens = whisper_full_n_tokens(_ctx, seg);
        for (int tok = 0; tok < nTokens; ++tok) {
            const char* rawText = whisper_full_get_token_text(_ctx, seg, tok);
            if (!rawText) continue;

            std::string tokStr(rawText);

            // Skip special / timing tokens (e.g. "[_BEG_]", "[_TT_xxx]")
            if (tokStr.size() >= 2 && tokStr.front() == '[' && tokStr.back() == ']') {
                if (inWord && !currentWord.text.empty()) {
                    currentWord.confidence = wordTokCount > 0
                                                ? wordProbSum / static_cast<float>(wordTokCount)
                                                : 0.0f;
                    words.push_back(currentWord);
                    inWord = false;
                }
                wordProbSum  = 0.0f;
                wordTokCount = 0;
                continue;
            }

            whisper_token_data td = whisper_full_get_token_data(_ctx, seg, tok);

            // A leading space signals a new word boundary.
            bool startsNewWord = (!tokStr.empty() && tokStr[0] == ' ');

            if (startsNewWord && inWord && !currentWord.text.empty()) {
                currentWord.confidence = wordTokCount > 0
                                            ? wordProbSum / static_cast<float>(wordTokCount)
                                            : 0.0f;
                words.push_back(currentWord);
                currentWord  = {};
                wordProbSum  = 0.0f;
                wordTokCount = 0;
                inWord       = false;
            }

            if (!inWord) {
                currentWord.startTimeMs = td.t0 * 10; // centiseconds → ms
                inWord = true;
            }

            // Strip the leading space before appending to the word text.
            if (!tokStr.empty() && tokStr[0] == ' ')
                tokStr = tokStr.substr(1);
            currentWord.text      += tokStr;
            currentWord.endTimeMs  = td.t1 * 10;

            wordProbSum  += td.p;
            wordTokCount += 1;
            totalProb    += td.p;
            tokenCount   += 1;
        }

        if (inWord && !currentWord.text.empty()) {
            currentWord.confidence = wordTokCount > 0
                                        ? wordProbSum / static_cast<float>(wordTokCount)
                                        : 0.0f;
            words.push_back(currentWord);
        }
    }

    // whisper.cpp segment text starts with a leading space; strip it.
    if (!fullText.empty() && fullText[0] == ' ')
        fullText = fullText.substr(1);

    result.text       = std::move(fullText);
    result.confidence = tokenCount > 0
                            ? totalProb / static_cast<float>(tokenCount)
                            : 0.0f;
    result.words      = std::move(words);
    return result;
}

// ---------------------------------------------------------------------------
// Inference loop (background thread)
// ---------------------------------------------------------------------------

void WhisperCppTranscriptionProvider::inferenceLoop()
{
    auto lastPartialTime = std::chrono::steady_clock::now();

    while (_running) {
        std::vector<float> audioSnapshot;
        bool doFinal   = false;
        bool doPartial = false;

        {
            std::unique_lock<std::mutex> lock(_mtx);
            _cv.wait_for(lock, std::chrono::milliseconds(10));

            if (!_running) break;
            if (_buffer.empty() || !_hasNewAudio) continue;

            auto now       = std::chrono::steady_clock::now();
            auto idleMs    = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - _lastWriteTime).count();
            auto partialMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - lastPartialTime).count();

            if (_endOfSpeech || idleMs >= _inferenceConfig.idleTimeoutMs) {
                audioSnapshot = std::move(_buffer);
                _buffer.clear();
                _hasNewAudio  = false;
                _endOfSpeech  = false;
                doFinal       = true;
            } else if (partialMs >= _inferenceConfig.partialIntervalMs) {
                audioSnapshot = _buffer; // copy, do not drain
                doPartial     = true;
            }
        }

        if (!(doFinal || doPartial) || audioSnapshot.empty()) continue;

        // Determine effective audio size (for finals triggered by commitEndOfSpeech
        // the buffer may have grown with silence padding after the speech boundary).
        size_t effectiveSize = (doFinal && _bufferSizeAtEndOfSpeech > 0)
                                   ? _bufferSizeAtEndOfSpeech
                                   : audioSnapshot.size();
        size_t growth = (_lastInferSamples > 0 && effectiveSize > _lastInferSamples)
                            ? effectiveSize - _lastInferSamples
                            : effectiveSize;

        bool canReuseCached = !_cachedPartialResult.empty() &&
                              (doFinal ? growth < _inferenceConfig.minNewSamplesForFinal
                                       : growth < _inferenceConfig.minNewSamplesForPartial);

        std::string text;
        float confidence = 0.0f;
        std::vector<va::audio::TranscriptionResult::Word> words;

        if (canReuseCached) {
            text       = _cachedPartialResult;
            confidence = _cachedPartialConfidence;
            words      = _cachedPartialWords;
            spdlog::debug("[WhisperCpp] Reusing cached result for {} (growth={} samples)",
                          doFinal ? "final" : "partial", growth);
        } else {
            // Trim audio to effectiveSize before passing to inference.
            std::vector<float> inferAudio(audioSnapshot.begin(),
                                          audioSnapshot.begin() +
                                              static_cast<ptrdiff_t>(
                                                  std::min(effectiveSize, audioSnapshot.size())));
            auto inferResult       = runInference(inferAudio);
            text                   = std::move(inferResult.text);
            confidence             = inferResult.confidence;
            words                  = std::move(inferResult.words);
            _lastInferSamples      = audioSnapshot.size();
            _cachedPartialResult   = text;
            _cachedPartialConfidence = confidence;
            _cachedPartialWords    = words;
        }

        if (!text.empty() && _listener) {
            va::audio::TranscriptionResult result;
            result.text       = text;
            result.confidence = confidence;
            result.words      = words;

            if (doFinal) {
                _listener->onFinalTranscription(result);
                _cachedPartialResult.clear();
                _cachedPartialConfidence = 0.0f;
                _cachedPartialWords.clear();
                _lastInferSamples       = 0;
                _bufferSizeAtEndOfSpeech = 0;
            } else {
                _listener->onPartialTranscription(result);
                lastPartialTime = std::chrono::steady_clock::now();
            }
        } else if (doPartial) {
            lastPartialTime = std::chrono::steady_clock::now();
        }
    }
}
