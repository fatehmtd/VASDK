#include "ParakeetTranscriptionProvider.hpp"
#include "impl/ParakeetInferenceProviderImpl.hpp"
#include <VASDK/AudioSIMD.h>
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>
#include <cstring>  // std::memcpy

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Convert raw PCM bytes to normalised float samples and write into out[0..outCapacity-1].
/// Returns the number of float samples written (capped at outCapacity).
/// This is the allocation-free core used by the hot streaming path.
static size_t convertToFloatInto(const uint8_t* data, size_t size,
                                  va::audio::AudioFormat format,
                                  float* out, size_t outCapacity)
{
    switch (format) {
    case va::audio::AudioFormat::SHORT: {
        if (size % sizeof(int16_t) != 0) {
            spdlog::warn("[Parakeet] SHORT audio size {} is not a multiple of 2; truncating", size);
            size -= size % sizeof(int16_t);
        }
        size_t n = std::min(size / sizeof(int16_t), outCapacity);
        AudioSIMD::convertInt16ToFloat(reinterpret_cast<const int16_t*>(data), out, n);
        return n;
    }
    case va::audio::AudioFormat::FLOAT: {
        if (size % sizeof(float) != 0) {
            spdlog::warn("[Parakeet] FLOAT audio size {} is not a multiple of 4; truncating", size);
            size -= size % sizeof(float);
        }
        size_t n = std::min(size / sizeof(float), outCapacity);
        std::memcpy(out, data, n * sizeof(float));
        return n;
    }
    case va::audio::AudioFormat::INT: {
        if (size % sizeof(int32_t) != 0) {
            spdlog::warn("[Parakeet] INT audio size {} is not a multiple of 4; truncating", size);
            size -= size % sizeof(int32_t);
        }
        size_t n = std::min(size / sizeof(int32_t), outCapacity);
        AudioSIMD::convertInt32ToFloat(reinterpret_cast<const int32_t*>(data), out, n);
        return n;
    }
    case va::audio::AudioFormat::BYTE: {
        size_t n = std::min(size, outCapacity);
        AudioSIMD::convertByteToFloat(data, out, n);
        return n;
    }
    default:
        spdlog::error("[Parakeet] Unknown audio format; cannot convert");
        return 0;
    }
}

/// Convenience wrapper that allocates a vector.
/// Used only in writeFullAudioData where a one-shot allocation is acceptable.
static std::vector<float> convertToFloat(const uint8_t* data, size_t size,
                                          va::audio::AudioFormat format)
{
    // Upper bound on sample count: BYTE gives 1 sample per byte.
    std::vector<float> out(size);
    size_t n = convertToFloatInto(data, size, format, out.data(), out.size());
    out.resize(n);
    return out;
}

// ---------------------------------------------------------------------------
// ParakeetTranscriptionProvider
// ---------------------------------------------------------------------------

ParakeetTranscriptionProvider::ParakeetTranscriptionProvider(const std::string& modelDir)
    : _modelDir(modelDir)
{
}

ParakeetTranscriptionProvider::~ParakeetTranscriptionProvider()
{
    try { if (_running) stopTranscriptionProvider(); } catch (...) {}
    releaseTranscriptionProvider();
}

bool ParakeetTranscriptionProvider::initializeTranscriptionProvider()
{
    if (_parakeetInferenceProviderImpl) {
        spdlog::warn("[Parakeet] Already initialized");
        return true;
    }

    _parakeetInferenceProviderImpl = std::make_unique<ParakeetInferenceProviderImpl>();
    if (!_parakeetInferenceProviderImpl->initialize(_modelDir, false)) {
        spdlog::error("[Parakeet] Failed to initialize inference engine");
        _parakeetInferenceProviderImpl.reset();
        return false;
    }

    spdlog::info("[Parakeet] Initialized with model from {}", _modelDir);
    return true;
}

void ParakeetTranscriptionProvider::releaseTranscriptionProvider()
{
    if (_parakeetInferenceProviderImpl) {
        _parakeetInferenceProviderImpl->release();
        _parakeetInferenceProviderImpl.reset();
    }
}

bool ParakeetTranscriptionProvider::startTranscriptionProvider()
{
    if (_running) return true;
    if (!_parakeetInferenceProviderImpl) {
        spdlog::error("[Parakeet] Not initialized, cannot start");
        return false;
    }

    // Preallocate both audio buffers once so that audio writes never trigger
    // heap allocations.  setAudioConfiguration() and setInferenceConfig() must
    // be called before start(); resizing after start() is not supported.
    if (_audioConfig.sampleRate <= 0) {
        spdlog::warn("[Parakeet] sampleRate not set before start - defaulting to 16000 Hz");
        _audioConfig.sampleRate = 16000;
    }
    const size_t maxSamples = static_cast<size_t>(
        _inferenceConfig.maxBufferSeconds * static_cast<float>(_audioConfig.sampleRate));
    _buffer.assign(maxSamples, 0.0f);
    _inferenceBuffer.assign(maxSamples, 0.0f);
    _bufferSize          = 0;
    _inferenceBufferSize = 0;
    _audioWithContext.clear();
    _audioWithContext.reserve(_inferenceConfig.contextSamples + maxSamples);

    _running       = true;
    _allowCallbacks = true;
    _hasNewAudio   = false;
    _lastWriteTime = std::chrono::steady_clock::now();

    _thread = std::thread(&ParakeetTranscriptionProvider::inferenceLoop, this);
    spdlog::info("[Parakeet] Started");
    return true;
}

void ParakeetTranscriptionProvider::stopTranscriptionProvider()
{
    if (!_running) return;

    _running = false;
    _allowCallbacks = false;
    _cv.notify_all();

    if (_thread.joinable()) _thread.join();

    std::lock_guard<std::mutex> lock(_mtx);
    std::fill(_buffer.begin(),          _buffer.begin()          + _bufferSize,          0.0f);
    std::fill(_inferenceBuffer.begin(), _inferenceBuffer.begin() + _inferenceBufferSize, 0.0f);
    _bufferSize               = 0;
    _inferenceBufferSize      = 0;
    _hasNewAudio              = false;
    _endOfSpeech              = false;
    _sampleCountAtLastPartial = 0;
    _bufferSizeAtEndOfSpeech  = 0;
    _contextTailBuffer.clear();

    spdlog::info("[Parakeet] Stopped");
}

bool ParakeetTranscriptionProvider::isTranscriptionProviderRunning() const
{
    return _running;
}

size_t ParakeetTranscriptionProvider::writeAudioData(const uint8_t* data, size_t size)
{
    if (!_running || !data || size == 0) return 0;

    {
        std::lock_guard<std::mutex> lock(_mtx);
        appendAudioAsFloat(data, size);
        _lastWriteTime = std::chrono::steady_clock::now();
        _hasNewAudio   = true;
    }
    _cv.notify_one();

    return size;
}

void ParakeetTranscriptionProvider::commitEndOfSpeech()
{
    {
        std::lock_guard<std::mutex> lock(_mtx);
        // Always signal end-of-speech regardless of current buffer state.
        // The original guard (_hasNewAudio && !_buffer.empty()) caused the
        // signal to be silently dropped if the inference thread had just
        // cleared the buffer moments before this call.
        _endOfSpeech             = true;
        _bufferSizeAtEndOfSpeech = _bufferSize;
    }
    _cv.notify_one();
}

va::audio::TranscriptionResult ParakeetTranscriptionProvider::writeFullAudioData(
    const uint8_t* data, size_t size)
{
    std::vector<float> audioFloats = convertToFloat(data, size, _audioConfig.format);

    // Parakeet TDT is designed for ~30-second segments.  For longer audio we split
    // into 60-second chunks.  Each chunk (except the first) overlaps 2 s into the
    // previous one so the decoder has prior context at every boundary.
    //
    // The first chunk prepends 0.5 s of silence as decoder warm-up: without it the
    // TDT decoder tends to output blanks for the first few words because it starts
    // without any prior-context frames.
    //
    // The last chunk gets 2 s of silence appended so the decoder can flush all
    // pending tokens before the buffer ends.  Words that start after the real audio
    // ends are discarded (with a tolerance for tokens spoken right at the boundary).

    const size_t totalSamples      = audioFloats.size();
    const size_t chunkSamples      = std::min(static_cast<size_t>(60 * _audioConfig.sampleRate), totalSamples);
    const size_t overlapSamples    = static_cast<size_t>(2.0f * _audioConfig.sampleRate);
    const size_t warmupSamples     = static_cast<size_t>(0.5f * _audioConfig.sampleRate);
    const size_t silencePadSamples = std::min(totalSamples, static_cast<size_t>(_inferenceConfig.silencePaddingSeconds * _audioConfig.sampleRate));

    const int64_t audioEndMs  = static_cast<int64_t>(totalSamples) * 1000 / _audioConfig.sampleRate;
    const size_t  totalChunks = (totalSamples + chunkSamples - 1) / chunkSamples;

    // Word boundary tolerances (milliseconds).
    // overlapBoundaryToleranceMs - how far before logicalStartMs a word may begin
    //   and still be included (handles tokens that straddle the chunk boundary).
    // endPaddingToleranceMs - how far past audioEndMs a word may begin on the last
    //   chunk (~12 encoder frames of runway to flush tokens for speech at the end).
    const int64_t overlapBoundaryToleranceMs = _inferenceConfig.overlapBoundaryToleranceMs;
    const int64_t endPaddingToleranceMs      = _inferenceConfig.endPaddingToleranceMs;

    std::string fullText;
    std::vector<va::audio::TranscriptionResult::LanguageTag> detectedLanguages;
    float totalConfidence = 0.0f;
    int   numChunks = 0;
    std::vector<va::audio::TranscriptionResult::Word> allWords;

    size_t logicalOffset = 0;
    while (logicalOffset < totalSamples) {
        const bool isFirstChunk = (logicalOffset == 0);
        const bool isLastChunk  = (logicalOffset + chunkSamples >= totalSamples);

        // The read window extends overlapSamples back into the previous chunk to
        // give the decoder prior context.  For the first chunk there is no previous
        // chunk; the warmup silence below serves the same role.
        const size_t readStart      = isFirstChunk ? 0 : logicalOffset - overlapSamples;
        const size_t overlapActual  = logicalOffset - readStart;
        const size_t logicalChunkSize = std::min(chunkSamples, totalSamples - logicalOffset);
        const size_t readEnd        = readStart + overlapActual + logicalChunkSize;

        const int64_t readStartMs    = static_cast<int64_t>(readStart)    * 1000 / _audioConfig.sampleRate;
        const int64_t logicalStartMs = static_cast<int64_t>(logicalOffset) * 1000 / _audioConfig.sampleRate;

        // The warmup silence prepended to the first chunk shifts all decoder
        // timestamps forward by warmupOffsetMs.  We subtract this when converting
        // chunk-relative timestamps back to absolute input-audio time.
        const int64_t warmupOffsetMs = isFirstChunk
            ? static_cast<int64_t>(warmupSamples) * 1000 / _audioConfig.sampleRate
            : 0;

        spdlog::debug("[Parakeet] writeFullAudioData: chunk {}/{} ({:.1f}s-{:.1f}s, {:.1f}s overlap{}{})",
            numChunks + 1, totalChunks,
            static_cast<float>(readStart) / _audioConfig.sampleRate,
            static_cast<float>(readEnd)   / _audioConfig.sampleRate,
            static_cast<float>(overlapActual) / _audioConfig.sampleRate,
            isFirstChunk ? ", +warmup"      : "",
            isLastChunk  ? ", +silence pad" : "");

        // Assemble the chunk: [warmup silence?] + [audio window] + [end silence?].
        std::vector<float> chunk;
        if (isFirstChunk)
            chunk.resize(warmupSamples, 0.0f);
        chunk.insert(chunk.end(), audioFloats.data() + readStart, audioFloats.data() + readEnd);
        if (isLastChunk)
            chunk.resize(chunk.size() + silencePadSamples, 0.0f);

        auto result = _parakeetInferenceProviderImpl->infer(chunk.data(), chunk.size(), _audioConfig.sampleRate);

        // Merge detected languages, keeping the highest confidence per code.
        for (const auto& lang : result.languages) {
            auto it = std::find_if(detectedLanguages.begin(), detectedLanguages.end(),
                [&](const auto& l) { return l.code == lang.code; });
            if (it == detectedLanguages.end())
                detectedLanguages.push_back(lang);
            else if (lang.confidence > it->confidence)
                it->confidence = lang.confidence;
        }

        // Convert chunk-relative timestamps to absolute input-audio timestamps,
        // then discard words outside the logical window for this chunk.
        //
        // Timestamps from infer() are relative to chunk start (including any warmup
        // silence).  To get absolute time: subtract warmupOffsetMs, then add readStartMs.
        std::string chunkText;
        for (auto& word : result.words) {
            word.startTimeMs += readStartMs - warmupOffsetMs;
            word.endTimeMs   += readStartMs - warmupOffsetMs;

            // Discard words from the overlap region (already emitted by previous chunk).
            if (word.startTimeMs < logicalStartMs - overlapBoundaryToleranceMs) continue;
            // On the last chunk, discard decoder hallucinations from the silence pad.
            if (isLastChunk && word.startTimeMs > audioEndMs + endPaddingToleranceMs) break;

            if (!chunkText.empty()) chunkText += ' ';
            chunkText += word.text;
            allWords.push_back(word);
        }

        if (!fullText.empty() && !chunkText.empty()) fullText += ' ';
        fullText += chunkText;

        totalConfidence += result.confidence;
        numChunks++;
        logicalOffset += logicalChunkSize;
    }

    va::audio::TranscriptionResult transcriptionResult;
    transcriptionResult.startTimeMs  = allWords.empty() ? 0 : allWords.front().startTimeMs;
    transcriptionResult.endTimeMs    = allWords.empty() ? 0 : allWords.back().endTimeMs;
    transcriptionResult.text         = fullText;
    transcriptionResult.confidence   = numChunks > 0 ? totalConfidence / numChunks : 0.0f;
    transcriptionResult.words        = std::move(allWords);
    transcriptionResult.languageTags = std::move(detectedLanguages);
    return transcriptionResult;
}

void ParakeetTranscriptionProvider::setTranscriptionResultListener(
    va::audio::TranscriptionResultListener* listener)
{
    _listener.store(listener, std::memory_order_release);
}

va::audio::TranscriptionResultListener* ParakeetTranscriptionProvider::getTranscriptionResultListener() const
{
    return _listener.load(std::memory_order_acquire);
}

void ParakeetTranscriptionProvider::setAudioConfiguration(const va::audio::AudioConfig& config)
{
    _audioConfig = config;
}

va::audio::AudioConfig ParakeetTranscriptionProvider::getAudioConfiguration() const
{
    return _audioConfig;
}

void ParakeetTranscriptionProvider::appendAudioAsFloat(const uint8_t* data, size_t size)
{
    const size_t available = _buffer.size() - _bufferSize;
    if (available == 0) {
        spdlog::warn("[Parakeet] Audio buffer full ({} samples), dropping incoming audio",
                     _buffer.size());
        return;
    }
    // Write converted samples directly into the preallocated buffer - no heap allocation.
    size_t written = convertToFloatInto(data, size, _audioConfig.format,
                                         _buffer.data() + _bufferSize, available);
    _bufferSize += written;
}

void ParakeetTranscriptionProvider::clearAudioBuffer()
{
    std::lock_guard<std::mutex> lock(_mtx);
    // Discard audio accumulated so far - it may contain TTS speaker echo captured
    // during barge-in.  We intentionally keep _contextTailBuffer: it holds clean
    // speech from the *previous* completed turn and remains useful decoder context.
    std::fill(_buffer.begin(), _buffer.begin() + _bufferSize, 0.0f);
    _bufferSize              = 0;
    _hasNewAudio             = false;
    _endOfSpeech             = false;
    _bufferSizeAtEndOfSpeech = 0;
}

void ParakeetTranscriptionProvider::setInferenceConfig(const InferenceConfig& config)
{
    std::lock_guard<std::mutex> lock(_mtx);
    _inferenceConfig = config;
}

ParakeetTranscriptionProvider::InferenceConfig ParakeetTranscriptionProvider::getInferenceConfig() const
{
    return _inferenceConfig;
}

void ParakeetTranscriptionProvider::setTimingObserver(va::metrics::TimingObserverInterface* observer)
{
    if (_parakeetInferenceProviderImpl)
        _parakeetInferenceProviderImpl->setTimingObserver(observer);
}

void ParakeetTranscriptionProvider::inferenceLoop()
{
    auto canEmitCallbacks = [this]() {
        return _allowCallbacks.load(std::memory_order_acquire);
    };

    auto getListener = [this, &canEmitCallbacks]() -> va::audio::TranscriptionResultListener* {
        if (!canEmitCallbacks()) {
            return nullptr;
        }
        return _listener.load(std::memory_order_acquire);
    };

    auto clearSegmentState = [this]() {
        _cachedPartialResult.clear();
        _cachedPartialLanguage.clear();
        _cachedPartialConfidence = 0.0f;
        _cachedPartialWords.clear();
        _lastInferSamples         = 0;
        _sampleCountAtLastPartial = 0;
        _bufferSizeAtEndOfSpeech  = 0;
    };

    auto lastPartialTime = std::chrono::steady_clock::now();

    while (_running) {
        bool doFinal   = false;
        bool doPartial = false;
        InferenceConfig cfg;

        {
            std::unique_lock<std::mutex> lock(_mtx);
            // Snapshot config first so idleTimeoutMs is available as the wait deadline.
            cfg = _inferenceConfig;
            // Wake immediately when audio arrives or speech ends; otherwise sleep
            // up to idleTimeoutMs (default 100 ms) instead of spinning at 10 ms.
            _cv.wait_for(lock, std::chrono::milliseconds(cfg.idleTimeoutMs), [this] {
                return !_running || _hasNewAudio || _endOfSpeech;
            });

            if (!_running) break;
            if ((_bufferSize == 0 || !_hasNewAudio) && !_endOfSpeech) continue;

            auto now            = std::chrono::steady_clock::now();
            auto idleMs         = std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastWriteTime).count();
            auto sincePartialMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPartialTime).count();

            if (_endOfSpeech || idleMs >= cfg.idleTimeoutMs) {
                // VAD signalled end-of-speech, or the input stream went quiet.
                // Swap accumulation and inference buffers in O(1), then zero-fill
                // the (now clean) accumulation buffer so stale samples can't leak.
                std::swap(_buffer, _inferenceBuffer);
                _inferenceBufferSize = _bufferSize;
                std::fill(_buffer.begin(), _buffer.begin() + _inferenceBufferSize, 0.0f);
                _bufferSize  = 0;
                _hasNewAudio = false;
                _endOfSpeech = false;
                doFinal      = true;
            }
            else if (sincePartialMs >= cfg.partialIntervalMs && !_endOfSpeech) {
                // Partial result: snapshot the valid size under the lock only -
                // the actual copy runs outside to keep _mtx hold-time short.
                // Skip if EOS is already pending - the shortcut will fire on the
                // next tick using the cached partial, avoiding a ~500 ms inference
                // stall that would delay the final result.
                _inferenceBufferSize = _bufferSize;
                doPartial            = true;
            }
        }

        // Copy partial audio outside the lock.  New audio is appended to
        // _buffer[_inferenceBufferSize..], so reading _buffer[0.._inferenceBufferSize)
        // is safe here without holding _mtx.
        if (doPartial && !doFinal) {
            std::copy(_buffer.begin(), _buffer.begin() + _inferenceBufferSize,
                      _inferenceBuffer.begin());
        }

        if (!(doFinal || doPartial) || !_parakeetInferenceProviderImpl)
            continue;

        // If the buffer is empty (e.g. idle timeout already drained it before
        // commitEndOfSpeech arrived), emit the cached partial as the final
        // result rather than silently dropping the end-of-speech signal.
        if (_inferenceBufferSize == 0) {
            if (doFinal) {
                va::audio::TranscriptionResult result;
                if (!_cachedPartialResult.empty()) {
                    spdlog::info("[Parakeet] End-of-speech with empty buffer - "
                                 "emitting cached partial as final");
                    result.text         = _cachedPartialResult;
                    result.confidence   = _cachedPartialConfidence;
                    result.words        = _cachedPartialWords;
                    result.languageTags = _cachedPartialLanguage;
                } else {
                    spdlog::warn("[Parakeet] End-of-speech with empty buffer and no cached partial - "
                                 "emitting empty final result to avoid a hanging turn");
                }

                auto* listener = getListener();
                if (listener) {
                    listener->onFinalTranscription(result);
                } else {
                    spdlog::warn("[Parakeet] Final result dropped because transcription listener is not available");
                }
                clearSegmentState();
            }
            continue;
        }

        VASDK_CTRACK_NAME("Parakeet::processUtterance");
        std::string text;
        std::vector<va::audio::TranscriptionResult::LanguageTag> language;
        float confidence = 0.0f;
        std::vector<va::audio::TranscriptionResult::Word> words;

        // Use the buffer size recorded at commitEndOfSpeech() as the effective audio
        // length for finals: samples appended after that point (e.g. pipeline silence
        // padding) don't carry speech and shouldn't force a re-inference.
        const size_t effectiveSize = (doFinal && _bufferSizeAtEndOfSpeech > 0)
            ? _bufferSizeAtEndOfSpeech
            : _inferenceBufferSize;
        const size_t growth = (_lastInferSamples > 0 && effectiveSize > _lastInferSamples)
            ? effectiveSize - _lastInferSamples
            : effectiveSize;

        // Re-use the last cached result when:
        //  a) the buffer barely grew since the last inference (trivial addition), OR
        //  b) final inference and the cached partial has high confidence with only a
        //     modest amount of NEW audio since that partial result was ready.
        //
        // For (b) we use _sampleCountAtLastPartial - the buffer size captured
        // AFTER the last partial inference finished - rather than _lastInferSamples
        // (captured BEFORE it started).  This excludes audio that accumulated
        // during the inference run itself, so the threshold only sees the true
        // "post-result" silence: typically just VAD PAUSED → ENDING → ENDED tail.
        const size_t shortcutGrowth = (_sampleCountAtLastPartial > 0 &&
                                       _bufferSizeAtEndOfSpeech >= _sampleCountAtLastPartial)
            ? _bufferSizeAtEndOfSpeech - _sampleCountAtLastPartial
            : _bufferSizeAtEndOfSpeech;
        const bool highConfShortcut = doFinal
            && cfg.highConfShortcutSamples > 0
            && _cachedPartialConfidence >= cfg.highConfShortcutThreshold
            && shortcutGrowth < cfg.highConfShortcutSamples;
        const bool canReuseCached = !_cachedPartialResult.empty() &&
            (doFinal ? (growth < cfg.minNewSamplesForFinal || highConfShortcut)
                     : growth < cfg.minNewSamplesForPartial);

        if (canReuseCached) {
            text       = _cachedPartialResult;
            language   = _cachedPartialLanguage;
            confidence = _cachedPartialConfidence;
            words      = _cachedPartialWords;
            if (highConfShortcut && growth >= cfg.minNewSamplesForFinal) {
                spdlog::info("[Parakeet] High-confidence final shortcut - skipping inference, "
                             "promoting cached partial as final "
                             "(conf={:.2f}, shortcutGrowth={} samples < {} threshold)",
                             _cachedPartialConfidence, shortcutGrowth, cfg.highConfShortcutSamples);
            } else {
                spdlog::debug("[Parakeet] Reusing cached result for {} (growth={} samples)",
                    doFinal ? "final" : "partial", growth);
            }

            if (doFinal) {
                // Keep the context tail current even when skipping re-inference.
                const size_t maxCtx = cfg.contextSamples;
                const float* begin  = _inferenceBuffer.data();
                const float* end    = begin + _inferenceBufferSize;
                _contextTailBuffer.assign(
                    _inferenceBufferSize >= maxCtx ? end - maxCtx : begin, end);
            }
        }
        else {
            VASDK_CTRACK_NAME("Parakeet::runInference");
            // For finals, prepend the tail of the previous segment so the decoder
            // has prior-speech context at hard VAD/idle boundaries.
            const float* inferPtr  = _inferenceBuffer.data();
            size_t       inferSize = _inferenceBufferSize;
            _audioWithContext.clear();  // O(1) - preserves pre-allocated capacity
            size_t contextSamples = 0;

            if (doFinal && !_contextTailBuffer.empty()) {
                contextSamples = _contextTailBuffer.size();
                _audioWithContext.insert(_audioWithContext.end(),
                    _contextTailBuffer.begin(), _contextTailBuffer.end());
                _audioWithContext.insert(_audioWithContext.end(),
                    _inferenceBuffer.begin(), _inferenceBuffer.begin() + _inferenceBufferSize);
                inferPtr  = _audioWithContext.data();
                inferSize = _audioWithContext.size();
            }

            const bool useIncrementalPartial = cfg.enableIncrementalPartials && doPartial && !doFinal;
            auto inferResult = _parakeetInferenceProviderImpl->infer(
                inferPtr, inferSize, _audioConfig.sampleRate, useIncrementalPartial);
            confidence = inferResult.confidence;
            language   = inferResult.languages;

            if (doFinal) {
                // Update the context tail before consuming the result.
                const size_t maxCtx = cfg.contextSamples;
                const float* begin  = _inferenceBuffer.data();
                const float* end    = begin + _inferenceBufferSize;
                _contextTailBuffer.assign(
                    _inferenceBufferSize >= maxCtx ? end - maxCtx : begin, end);

                if (contextSamples > 0) {
                    // Strip words that belong to the context prefix and convert
                    // timestamps to segment-relative (context-free) time.
                    //
                    // Strategy depends on whether we have a cached partial:
                    //  • With cached partial  → strict boundary (contextMs).  Any
                    //    leading words the model placed slightly before the boundary
                    //    are recovered from the cached partial below.  This prevents
                    //    trailing words of the previous turn from bleeding in.
                    //  • Without cached partial (utterance shorter than the partial
                    //    interval) → small tolerance so the first word isn't lost.
                    const int64_t contextMs  = static_cast<int64_t>(contextSamples) * 1000
                                              / _audioConfig.sampleRate;
                    const bool hasCachedPartial = !_cachedPartialWords.empty();
                    const int64_t boundary = hasCachedPartial
                        ? contextMs          // strict: fallback recovers dropped leaders
                        : contextMs - 100;   // 100ms tolerance for short utterances

                    for (auto& word : inferResult.words) {
                        if (word.startTimeMs >= boundary) {
                            word.startTimeMs -= contextMs;
                            word.endTimeMs   -= contextMs;
                            if (!text.empty()) text += ' ';
                            text += word.text;
                            words.push_back(word);
                        }
                    }

                    // Recover leading words the model placed in the context region.
                    // The cached partial was computed without context so its leading
                    // words are the authoritative start of the new segment.
                    if (hasCachedPartial) {
                        if (words.empty()) {
                            // Every word landed in the context region; use cached partial.
                            text  = _cachedPartialResult;
                            words = _cachedPartialWords;
                        } else {
                            // Find where the filtered result's first word appears in
                            // the cached partial.  Words before that index were placed
                            // in the context region and must be prepended.
                            size_t matchIdx = _cachedPartialWords.size(); // sentinel = not found
                            for (size_t i = 0; i < _cachedPartialWords.size(); ++i) {
                                if (_cachedPartialWords[i].text == words.front().text) {
                                    matchIdx = i;
                                    break;
                                }
                            }
                            if (matchIdx > 0 && matchIdx < _cachedPartialWords.size()) {
                                std::string prefixText;
                                for (size_t i = 0; i < matchIdx; ++i) {
                                    if (!prefixText.empty()) prefixText += ' ';
                                    prefixText += _cachedPartialWords[i].text;
                                }
                                words.insert(words.begin(),
                                             _cachedPartialWords.begin(),
                                             _cachedPartialWords.begin() + matchIdx);
                                text = prefixText + ' ' + text;
                            }
                        }

                        // Stage 3: Word-level divergence anchoring.
                        // When context prepending causes the model to decode a
                        // different first word than the partial (e.g. "Call" →
                        // "Fall"), the cached partial - computed without context
                        // - is authoritative.  Substitute only when the cached
                        // partial's word-level confidence is strictly higher.
                        if (!words.empty() && !_cachedPartialWords.empty()
                            && words.front().text != _cachedPartialWords.front().text
                            && _cachedPartialWords.front().confidence > words.front().confidence) {
                            spdlog::info("[Parakeet] Boundary word substituted: "
                                         "final='{}' (conf={:.1f}%) → partial='{}' (conf={:.1f}%)",
                                         words.front().text,
                                         words.front().confidence * 100.0f,
                                         _cachedPartialWords.front().text,
                                         _cachedPartialWords.front().confidence * 100.0f);
                            words.front().text       = _cachedPartialWords.front().text;
                            words.front().confidence = _cachedPartialWords.front().confidence;
                            text.clear();
                            for (const auto& w : words) {
                                if (!text.empty()) text += ' ';
                                text += w.text;
                            }
                        }
                    }
                }
                else {
                    text  = inferResult.text;
                    words = std::move(inferResult.words);
                }
            }
            else {
                // Partial: no context prefix, use result directly.
                text  = inferResult.text;
                words = std::move(inferResult.words);
            }

            _lastInferSamples = _inferenceBufferSize;
            // Never let an empty result erase a non-empty cached partial: partials
            // keep running on a segment's trailing silence, and both the
            // context-boundary word recovery above and the empty-final safety net
            // below depend on the cached text surviving until the final.
            if (!text.empty()) {
                _cachedPartialResult     = text;
                _cachedPartialLanguage   = language;
                _cachedPartialConfidence = confidence;
                _cachedPartialWords      = words;
            }

            // For partials, record _bufferSize AFTER inference finished so the
            // high-confidence shortcut can measure only the audio that arrived
            // after this result was ready (not the audio that accumulated during
            // the inference run itself, which _lastInferSamples would include).
            if (!doFinal) {
                std::lock_guard<std::mutex> sampLock(_mtx);
                _sampleCountAtLastPartial = _bufferSize;
            }
        }

        // Final safety net: context-boundary stripping or inference failure may
        // produce empty text even though speech was detected (e.g. the model
        // assigned all word timestamps to the context prefix region and
        // _cachedPartialWords was empty so the normal hasCachedPartial recovery
        // branch did not fire).  Promote the cached partial text rather than
        // silently swallowing the end-of-speech signal - this prevents
        // LLMProcessor from waiting the full watchdog timeout (2 s) before
        // falling back to the same text anyway.
        if (doFinal && text.empty() && !_cachedPartialResult.empty()) {
            spdlog::info("[Parakeet] Final inference yielded empty text - "
                         "promoting cached partial as final: \"{}\" (conf={:.2f})",
                         _cachedPartialResult.substr(0, std::min<size_t>(60, _cachedPartialResult.size())),
                         _cachedPartialConfidence);
            text       = _cachedPartialResult;
            words      = _cachedPartialWords;
            confidence = _cachedPartialConfidence;
            language   = _cachedPartialLanguage;
        }

        if (doFinal) {
            va::audio::TranscriptionResult result;
            result.text         = text;
            result.confidence   = confidence;
            result.words        = words;
            result.languageTags = language;

            if (result.text.empty()) {
                spdlog::warn("[Parakeet] Final inference produced empty text - emitting empty final result");
            }

            auto* listener = getListener();
            if (listener) {
                listener->onFinalTranscription(result);
            } else {
                spdlog::warn("[Parakeet] Final result dropped because transcription listener is not available");
            }

            // Reset all per-segment streaming state for the next utterance.
            clearSegmentState();
        }
        else if (!text.empty()) {
            auto* listener = getListener();
            if (listener) {
                va::audio::TranscriptionResult result;
                result.text         = text;
                result.confidence   = confidence;
                result.words        = words;
                result.languageTags = language;
                listener->onPartialTranscription(result);
            }
        }

        // Reset the partial timer after every inference attempt so that a final
        // result doesn't immediately trigger a spurious partial on the next tick.
        lastPartialTime = std::chrono::steady_clock::now();
    }
}
