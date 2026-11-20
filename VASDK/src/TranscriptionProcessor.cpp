#include "VASDK/TranscriptionProcessor.h"
#include <VASDK/profiling/CTrack.h>
#include <VASDK/WavFile.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include "TranscriptionProcessor.h"

namespace va {
    namespace pipeline {

        // TranscriptionFrameProcessor
        TranscriptionFrameProcessor::TranscriptionFrameProcessor(
            std::shared_ptr<audio::GenericTranscriptionProvider> provider)
            : _transcriptionProvider(provider) {
        }

        bool TranscriptionFrameProcessor::initialize() {
            if (!FrameProcessor::initialize()) return false;
            if (!_transcriptionProvider) {
                spdlog::error("[TranscriptionFrameProcessor] No transcription provider");
                return false;
            }
            _transcriptionProvider->setTranscriptionResultListener(this);

            // Pre-register metrics
            if (_metrics.config.enabled && _collector) {
                using namespace va::metrics;
                auto winOrDefault = [&](uint32_t w) -> uint32_t {
                    return w > 0 ? w : 0u;
                    };
                _metrics.encoderLatency.initialize(_collector,
                    MetricDescriptor{ "transcription_encoder_ms", MetricType::Sampler,
                                     "Parakeet encoder inference latency (ms)",
                                     winOrDefault(_metrics.config.encoderWindow) });
                _metrics.decoderLatency.initialize(_collector,
                    MetricDescriptor{ "transcription_decoder_ms", MetricType::Sampler,
                                     "Parakeet decoder inference latency (ms)",
                                     winOrDefault(_metrics.config.decoderWindow) });
                _metrics.confidence.initialize(_collector,
                    MetricDescriptor{ "transcription_confidence_pct", MetricType::Sampler,
                                     "Final transcription confidence (0-100)",
                                     winOrDefault(_metrics.config.confidenceWindow) });
                _metrics.wordCount.initialize(_collector,
                    MetricDescriptor{ "transcription_word_count", MetricType::Sampler,
                                     "Word count per final transcription",
                                     winOrDefault(_metrics.config.wordCountWindow) });
                _metrics.transcriptions.initialize(_collector,
                    MetricDescriptor{ "transcriptions_total", MetricType::Counter,
                                     "Total final transcriptions produced" });
                // Register self as timing observer on the provider
                _transcriptionProvider->setTimingObserver(this);
            }

            return _transcriptionProvider->initializeTranscriptionProvider();
        }

        bool TranscriptionFrameProcessor::start() {
            if (!FrameProcessor::start()) return false;
            return _transcriptionProvider->startTranscriptionProvider();
        }

        void TranscriptionFrameProcessor::stop() {
            if (_transcriptionProvider) _transcriptionProvider->stopTranscriptionProvider();
            FrameProcessor::stop();
        }

        bool TranscriptionFrameProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const {
            switch (frame->getType()) {
            case transport::FrameType::AUDIO:
            case transport::FrameType::VAD_STATE:
            case transport::FrameType::CONTROL:   // TurnPolicyFrame, going up
            return true;
            default:
            return false;
            }
        }

        void TranscriptionFrameProcessor::setAudioDumpDirectory(const std::string& directory)
        {
            _audioDumpDirectory = directory;
            _audioDumpCount = 0;
            if (directory.empty()) return;
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) spdlog::warn("[TranscriptionFrameProcessor] Can't create {}: {}", directory, error.message());
            const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::ostringstream prefix;
            prefix << "stt-" << std::put_time(std::localtime(&now), "%Y%m%d-%H%M%S");
            _audioDumpPrefix = prefix.str();
            spdlog::warn("[TranscriptionFrameProcessor] Writing the audio sent to STT to {} (debugging: the user's voice on disk)", directory);
        }

        void TranscriptionFrameProcessor::dumpAudio(const std::vector<char>& audio)
        {
            if (_audioDumpDirectory.empty() || audio.empty() || !_transcriptionProvider) return;
            if (_privateAnswer) {
                spdlog::info("[TranscriptionFrameProcessor] Not saving the STT's audio: a private answer");
                return;
            }
            const auto config = _transcriptionProvider->getAudioConfiguration();
            std::vector<float> samples;
            if (config.format == audio::AudioFormat::SHORT) {
                samples = audio::fromPcm16(reinterpret_cast<const short*>(audio.data()), audio.size() / sizeof(short));
            } else if (config.format == audio::AudioFormat::FLOAT) {
                samples.resize(audio.size() / sizeof(float));
                std::memcpy(samples.data(), audio.data(), samples.size() * sizeof(float));
            } else {
                spdlog::warn("[TranscriptionFrameProcessor] Not saving the STT's audio: only 16-bit and float samples are written");
                return;
            }
            char name[32];
            std::snprintf(name, sizeof(name), "-%04d.wav", ++_audioDumpCount);
            const auto path = (std::filesystem::path(_audioDumpDirectory) / (_audioDumpPrefix + name)).string();
            std::string error;
            const int channels = config.channels > 0 ? config.channels : 1;
            if (audio::writeWav(path, samples.data(), samples.size(), config.sampleRate, channels, audio::AudioFormat::SHORT, &error))
                spdlog::info("[TranscriptionFrameProcessor] Saved the STT's audio: {} ({:.2f} s)", path,
                             static_cast<double>(samples.size()) / channels / std::max(1, config.sampleRate));
            else
                spdlog::warn("[TranscriptionFrameProcessor] Couldn't save the STT's audio to {}: {}", path, error);
        }

        void TranscriptionFrameProcessor::sendSilence(int durationMS) const
        {
            if (!_transcriptionProvider) {
                spdlog::warn("[TranscriptionFrameProcessor] Cannot send silence - no transcription provider");
                return;
            }
            auto audioConfig = _transcriptionProvider->getAudioConfiguration();
            std::vector<char> finalAudioData;
            finalAudioData.resize((audioConfig.getSampleSizeInBytes() * audioConfig.sampleRate * durationMS) / 1000.0); // durationMS milliseconds of audio
            for (size_t i = 0; i < finalAudioData.size(); i++) {
                finalAudioData[i] = 0;
            }
            // Write silence to flush the transcription provider and trigger final results
            _transcriptionProvider->writeAudioData(
                reinterpret_cast<const uint8_t*>(finalAudioData.data()),
                finalAudioData.size());
        }

        void TranscriptionFrameProcessor::flushPendingPreSpeech(const transport::AudioFrame& audioFrame)
        {
            _hasPendingPreSpeech = false;
            const long preSpeechSamples = _ringBuffer->computeDeltaOffset(
                static_cast<long>(_pendingPreSpeechOffset),
                static_cast<long>(audioFrame.ringbufferOffset));
            // A delta over half the ring means the start is ahead of the frame
            // (offset mismatch) - skip rather than send a huge stale chunk.
            if (preSpeechSamples <= 0 || preSpeechSamples >= _ringBuffer->getCapacity() / 2) {
                return;
            }
            const size_t preBytes = static_cast<size_t>(preSpeechSamples) * audioFrame.sampleSizeInBytes;
            _transcriptionBuffer.resize(preBytes);
            if (_ringBuffer->readAbsolute(static_cast<long>(_pendingPreSpeechOffset),
                _transcriptionBuffer.data(), 0,
                static_cast<int>(preSpeechSamples)) > 0) {
                _transcriptionProvider->writeAudioData(
                    reinterpret_cast<const uint8_t*>(_transcriptionBuffer.data()),
                    preBytes);
                if (!_audioDumpDirectory.empty())
                    _audioDumpSegment.insert(_audioDumpSegment.end(), _transcriptionBuffer.begin(), _transcriptionBuffer.end());
            }
            else {
                spdlog::warn("[TranscriptionFrameProcessor] Could not read pre-speech "
                    "audio from ring buffer (offset={}, length={})",
                    _pendingPreSpeechOffset, preSpeechSamples);
            }
        }

        void TranscriptionFrameProcessor::processAudioFrame(std::shared_ptr<transport::AudioFrame> audioFrame)
        {
            if (audioFrame && _transcriptionProvider && _ringBuffer) {
                if (_hasPendingPreSpeech) {
                    flushPendingPreSpeech(*audioFrame);
                }

                size_t bytesToRead = audioFrame->getSizeInBytes();
                _transcriptionBuffer.resize(bytesToRead);

                if (_ringBuffer->readAbsolute(audioFrame->ringbufferOffset,
                    _transcriptionBuffer.data(), 0,
                    audioFrame->numSamples) > 0) {
                    _transcriptionProvider->writeAudioData(
                        reinterpret_cast<const uint8_t*>(_transcriptionBuffer.data()),
                        bytesToRead);
                    if (!_audioDumpDirectory.empty() && _collectingAudio)
                        _audioDumpSegment.insert(_audioDumpSegment.end(), _transcriptionBuffer.begin(),
                                                 _transcriptionBuffer.begin() + static_cast<std::ptrdiff_t>(bytesToRead));
                }
                else {
                    spdlog::warn("[TranscriptionFrameProcessor] could not read audio data from ring buffer at offset {}", audioFrame->ringbufferOffset);
                }
            }
        }

        void TranscriptionFrameProcessor::processVADFrame(std::shared_ptr<transport::VADStateFrame> vadFrame)
        {
            if (isStreamingMode()) {
                processVADFrameStreaming(vadFrame);
            }
            else {
                processVADFrameNonStreaming(vadFrame);
            }
        }

        void TranscriptionFrameProcessor::processVADFrameStreaming(std::shared_ptr<transport::VADStateFrame> vadFrame)
        {
            switch (vadFrame->state) {
            case transport::VADState::STARTING: {
                // Clear partial cache for this new speech segment
                {
                    std::lock_guard<std::mutex> lock(_latestPartialMutex);
                    _latestPartialText.clear();
                    _latestPartialConfidence = 0.0f;
                }
                // Speech started: the pre-speech context (lookback) lives in the ring buffer
                // from speechStartOffset up to the AUDIO frame that triggered detection, which
                // arrives right after this VAD frame. Defer the read until then so we send
                // [speechStart, frameStart) followed by the frame itself - contiguous audio.
                // (totalSpeechLength only covers the detected speech, not the lookback gap,
                // so reading it here dropped the audio right before the onset.)
                _hasPendingPreSpeech = true;
                _pendingPreSpeechOffset = vadFrame->speechStartOffset;
                _collectingAudio = true;
                _audioDumpSegment.clear();
            } break;

            case transport::VADState::ENDED: {
                // Speech ended - commit what is in the provider's buffer as a final result.
                _collectingAudio = false;
                if (_transcriptionProvider) {
                    _transcriptionProvider->commitEndOfSpeech();
                }
                dumpAudio(_audioDumpSegment);
                _audioDumpSegment.clear();
            } break;

            case transport::VADState::QUIET: {
                _hasPendingPreSpeech = false;
                _audioDumpSegment.clear();   // speech too short to transcribe: dropped
                if (_collectingAudio) {
                    _collectingAudio = false;
                    if (_transcriptionProvider) {
                        _transcriptionProvider->clearAudioBuffer();
                    }
                }
            } break;

            default:
            // Other states: ongoing audio is handled by processAudioFrame().
            break;
            }
        }

        void TranscriptionFrameProcessor::processVADFrameNonStreaming(std::shared_ptr<transport::VADStateFrame> vadFrame)
        {
            switch (vadFrame->state)
            {
            case transport::VADState::STARTING: {
                spdlog::info("[TranscriptionFrameProcessor] VAD started - beginning to buffer audio ({} samples)",
                    vadFrame->totalSpeechLength);
            } break;
            case transport::VADState::PAUSED: {
                spdlog::info("[TranscriptionFrameProcessor] VAD paused");
            } break;
            case transport::VADState::RESUMING: {
                spdlog::info("[TranscriptionFrameProcessor] VAD resuming");
            } break;
            case transport::VADState::ENDING: {
                spdlog::info("[TranscriptionFrameProcessor] VAD ending");
            } break;
            case transport::VADState::ENDED:{
                // Non-streaming: the provider isn't fed incrementally. On end-of-speech,
                // read the full segment from the ring buffer and transcribe it in one shot.
                spdlog::info("[TranscriptionFrameProcessor] VAD ended - transcribing full segment ({} samples, {} seconds)",
                    vadFrame->totalSpeechLength, vadFrame->totalSpeechLength / static_cast<double>(_transcriptionProvider->getAudioConfiguration().sampleRate));

                const size_t bytesPerSample =
                    _transcriptionProvider->getAudioConfiguration().getSampleSizeInBytes();
                std::vector<char> finalAudioData(vadFrame->totalSpeechLength * bytesPerSample);

                if (_ringBuffer->readAbsolute(vadFrame->speechStartOffset,
                    finalAudioData.data(), 0,
                    vadFrame->totalSpeechLength) > 0)
                {
                    dumpAudio(finalAudioData);
                    auto transcriptionResult = _transcriptionProvider->writeFullAudioData(
                        reinterpret_cast<const uint8_t*>(finalAudioData.data()),
                        finalAudioData.size());
                    onFinalTranscription(transcriptionResult);
                }
                else {
                    spdlog::warn("[TranscriptionFrameProcessor] Could not read speech segment from "
                        "ring buffer (offset={}, length={})",
                        vadFrame->speechStartOffset, vadFrame->totalSpeechLength);
                }
            } break;
            default:
            break;
            }
        }

        void TranscriptionFrameProcessor::setMetricsConfig(const va::metrics::TranscriptionMetricsConfig& config)
        {
            _metrics.config = config;
            FrameProcessor::setMetricsConfig(static_cast<const va::metrics::ProcessorMetricsConfig&>(config));
        }

        void TranscriptionFrameProcessor::onTiming(const char* stage, double elapsedMs) noexcept
        {
            if (std::string_view(stage) == va::metrics::timing_stages::kEncoder)
                _metrics.encoderLatency.observe(elapsedMs);
            else if (std::string_view(stage) == va::metrics::timing_stages::kDecoder)
                _metrics.decoderLatency.observe(elapsedMs);
        }

        void TranscriptionFrameProcessor::configSilenceBehavior(bool atStart, bool atEnd, int durationMS)
        {
            _sendEndSilence = atEnd;
            _sendStartSilence = atStart;
            _silenceDurationMS = durationMS;
        }

        void TranscriptionFrameProcessor::processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) {
            VASDK_CTRACK_NAME("STT::processFrame");
            if (direction == FlowDirection::UPSTREAM) {   // words the next answer may hold
                if (auto policy = std::dynamic_pointer_cast<transport::TurnPolicyFrame>(frame)) {
                    _privateAnswer.store(policy->privateAnswer);
                    if (_transcriptionProvider) _transcriptionProvider->setContextHints(policy->hints);
                }
                pushUpstream(frame);
                return;
            }
            if (direction == FlowDirection::DOWNSTREAM) {
                if (!_collectingAudio) applyReplacement();
                switch (frame->getType()) {
                case transport::FrameType::AUDIO: {
                    if (_collectingAudio) {
                        auto audioFrame = std::dynamic_pointer_cast<transport::AudioFrame>(frame);
                        processAudioFrame(audioFrame);
                    }
                } break;
                case transport::FrameType::VAD_STATE: {
                    auto vadFrame = std::dynamic_pointer_cast<transport::VADStateFrame>(frame);
                    if (vadFrame) {
                        processVADFrame(vadFrame);
                    }
                } break;
                }
            }
            pushDownstream(frame);
        }

        // TranscriptionResultListener implementation
        void TranscriptionFrameProcessor::onTranscriptionStarted() {
            spdlog::info("[TranscriptionFrameProcessor] Transcription started");
        }

        void TranscriptionFrameProcessor::onTranscriptionStopped() {
            spdlog::info("[TranscriptionFrameProcessor] Transcription stopped");
        }

        void TranscriptionFrameProcessor::onTranscriptionError(const std::string& error) {
            spdlog::error("[TranscriptionFrameProcessor] Transcription error: {}", error);
            if (_onProviderError) _onProviderError(error);
        }

        void TranscriptionFrameProcessor::replaceProvider(std::shared_ptr<audio::GenericTranscriptionProvider> provider) {
            std::lock_guard<std::mutex> lock(_providerMutex);
            _replacement = provider == _transcriptionProvider ? nullptr : std::move(provider);
        }

        // On the pipeline thread, between turns.
        void TranscriptionFrameProcessor::applyReplacement() {
            std::shared_ptr<audio::GenericTranscriptionProvider> next, previous;
            {
                std::lock_guard<std::mutex> lock(_providerMutex);
                if (!_replacement) return;
                next = std::move(_replacement);
                _replacement.reset();
                previous = _transcriptionProvider;
            }
            if (previous) previous->stopTranscriptionProvider();
            next->setTranscriptionResultListener(this);
            if (_metrics.config.enabled && _collector) next->setTimingObserver(this);
            if (!next->initializeTranscriptionProvider() || !next->startTranscriptionProvider()) {
                spdlog::error("[TranscriptionFrameProcessor] {} failed to start; keeping {}", next->getName(),
                              previous ? previous->getName() : std::string("none"));
                if (previous) previous->startTranscriptionProvider();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(_providerMutex);
                _transcriptionProvider = next;
            }
            spdlog::info("[TranscriptionFrameProcessor] now transcribing with {}", next->getName());
        }

        void TranscriptionFrameProcessor::onPartialTranscription(const audio::TranscriptionResult& result) {
            // Cache for LLMProcessor's early-fire query; skip empty text so trailing-silence
            // partials don't clobber the last good hypothesis.
            if (!result.text.empty()) {
                std::lock_guard<std::mutex> lock(_latestPartialMutex);
                _latestPartialText = result.text;
                _latestPartialConfidence = result.confidence;
            }
            auto transcriptionFrame = std::make_shared<transport::TranscriptionFrame>();
            transcriptionFrame->isFinal = false;
            transcriptionFrame->sensitive = _privateAnswer.load();
            transcriptionFrame->transcriptionResult = result;
            transcriptionFrame->timestamp = transport::Frame::getCurrentTimestamp();
            transcriptionFrame->sourceId = getName();
            pushDownstream(transcriptionFrame);
        }

        void TranscriptionFrameProcessor::onFinalTranscription(const audio::TranscriptionResult& result) {
            // Clear partial cache - final supersedes it
            {
                std::lock_guard<std::mutex> lock(_latestPartialMutex);
                _latestPartialText.clear();
                _latestPartialConfidence = 0.0f;
            }
            _metrics.confidence.observe(static_cast<double>(result.confidence) * 100.0);
            _metrics.wordCount.observe(static_cast<double>(result.words.size()));
            _metrics.transcriptions.increment();

            auto transcriptionFrame = std::make_shared<transport::TranscriptionFrame>();
            transcriptionFrame->isFinal = true;
            transcriptionFrame->sensitive = _privateAnswer.load();
            transcriptionFrame->transcriptionResult = result;
            transcriptionFrame->timestamp = transport::Frame::getCurrentTimestamp();
            transcriptionFrame->sourceId = getName();
            pushDownstream(transcriptionFrame);
        }

        std::string TranscriptionFrameProcessor::getLatestPartialText() const {
            std::lock_guard<std::mutex> lock(_latestPartialMutex);
            return _latestPartialText;
        }

        float TranscriptionFrameProcessor::getLatestPartialConfidence() const {
            std::lock_guard<std::mutex> lock(_latestPartialMutex);
            return _latestPartialConfidence;
        }

    } // namespace pipeline
} // namespace va
