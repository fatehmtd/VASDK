#include "VASDK/VADFrameProcessor.h"
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>
#include <chrono>

namespace va {
    namespace pipeline {

        VADFrameProcessor::VADFrameProcessor(std::shared_ptr<audio::VADProvider> vadProvider, float lookbackMultiplier)
            : _vadProvider(vadProvider), _lookbackMultiplier(lookbackMultiplier) {
        }

        bool VADFrameProcessor::initialize() {
            if (!FrameProcessor::initialize()) return false;
            if (!_vadProvider) {
                spdlog::error("[VADFrameProcessor] No VAD provider");
                return false;
            }

            // Pre-register VAD-specific metrics.
            if (_metrics.config.enabled && _collector) {
                va::metrics::MetricDescriptor infDesc{
                    "vad_inference_ms",
                    va::metrics::MetricType::Sampler,
                    "VAD inference latency",
                    _metrics.config.inferenceWindow};
                va::metrics::MetricDescriptor durDesc{
                    "vad_speech_duration_ms",
                    va::metrics::MetricType::Sampler,
                    "Completed speech segment duration",
                    _metrics.config.speechDurWindow};
                va::metrics::MetricDescriptor segDesc{
                    "vad_segments_total",
                    va::metrics::MetricType::Counter,
                    "Completed speech segments"};

                _metrics.inference.initialize(_collector, infDesc);
                _metrics.speechDur.initialize(_collector, durDesc);
                _metrics.segments.initialize(_collector, segDesc);

                // Optional alert threshold on inference latency.
                if (_metrics.config.inferenceAlertMs > 0.0) {
                    va::metrics::MetricDescriptor gaugeDesc{
                        "vad_inference_alert",
                        va::metrics::MetricType::Gauge,
                        "VAD inference latency alert"};
                    _metrics.inferenceAlert.initialize(_collector, gaugeDesc);
                    _metrics.inferenceAlert.setThreshold(_metrics.config.inferenceAlertMs,
                                                         _collector->alertDispatcher());
                }
            }

            // VAD provider should already be initialized by the caller
            return true;
        }

        void VADFrameProcessor::setMetricsConfig(const va::metrics::VADMetricsConfig& config)
        {
            _metrics.config = config;
            FrameProcessor::setMetricsConfig(static_cast<const va::metrics::ProcessorMetricsConfig&>(config));
        }

        void VADFrameProcessor::setMinFrameSize(size_t samples)
        {
            _minFrameSizeSamples = samples;
        }

        bool VADFrameProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const {
            switch (frame->getType()) {
            case transport::FrameType::AUDIO:
            case transport::FrameType::CONTROL: // process to handle pipeline state changes (e.g. flush events)
            return true;
            default:
            return false;
            }
        }

        void VADFrameProcessor::processAudioFrame(std::shared_ptr<transport::AudioFrame> audioFrame)
        {
            VASDK_CTRACK_NAME("VAD::processAudioFrame");
            size_t bytesToRead = audioFrame->getSizeInBytes();
            size_t bufferSize = bytesToRead;

            _vadAudioBuffer.resize(bufferSize);

            int effectiveSamplesToRead = static_cast<int>(bufferSize / audioFrame->sampleSizeInBytes);

            // Look back far enough to capture the start of speech, if needed
            int64_t localAbsOffset = _ringBuffer->normalizeOffset((int64_t)audioFrame->postWriteOffset - effectiveSamplesToRead);

            int numSamplesRead = _ringBuffer->readAbsolute(localAbsOffset,
                _vadAudioBuffer.data(),
                0,
                effectiveSamplesToRead);

            if (numSamplesRead <= 0) {
                spdlog::warn("[VADFrameProcessor] Could not read audio data from ring buffer for VAD processing (offset: {}, requested: {} samples)",
                    localAbsOffset, effectiveSamplesToRead);
                return;
            }

            _absoluteAudioPreOffset = _absoluteAudioOffset;
            _absoluteAudioOffset += numSamplesRead;

            bool hasVoice = false;
            std::vector<va::audio::VADProvider::VADResult> vadResults;
        {
            va::metrics::ScopedTimer t(_metrics.inference.get());
            vadResults = _vadProvider->detectVoiceActivity(_vadAudioBuffer.data(), 0, static_cast<size_t>(numSamplesRead));
        }
            const int pauseWindowLengthSamples = static_cast<int>(
                _vadConfig.vadThresholdMs * _vadProvider->getAudioConfig().sampleRate / 1000);
            std::vector<va::audio::VADProvider::VADResult> mergedVadResults = mergeVadResults(vadResults, pauseWindowLengthSamples);

            if (!mergedVadResults.empty()) {
                for (auto& result : mergedVadResults) {
                    switch (_currentState) {
                    case transport::VADState::QUIET: {
                        processQuiet(&result);
                    } break;
                    case transport::VADState::STARTING: {
                        processStarting(&result);
                    } break;
                    case transport::VADState::ONGOING: {
                        processOngoing(&result);
                    } break;
                    case transport::VADState::PAUSED: {
                        processPaused(&result);
                    } break;
                    case transport::VADState::RESUMING: {
                        processResuming(&result);
                    } break;
                    case transport::VADState::ENDING: {
                        processEnding(&result);
                    } break;
                    case transport::VADState::ENDED: {
                        processEnded(&result);
                    } break;
                    }
                }
            }
            else {
                // No voice activity: still may need timing-based transitions (e.g. PAUSED to ENDING after a timeout)
                switch (_currentState) {
                case transport::VADState::QUIET: {
                    processQuiet(nullptr);
                } break;
                case transport::VADState::STARTING: {
                    processStarting(nullptr);
                } break;
                case transport::VADState::ONGOING: {
                    processOngoing(nullptr);
                } break;
                case transport::VADState::PAUSED: {
                    processPaused(nullptr);
                } break;
                case transport::VADState::RESUMING: {
                    processResuming(nullptr);
                } break;
                case transport::VADState::ENDING: {
                    processEnding(nullptr);
                } break;
                case transport::VADState::ENDED: {
                    processEnded(nullptr);
                } break;
                }
            }
        }

        void VADFrameProcessor::processControlFrame(std::shared_ptr<transport::ControlFrame> controlFrame)
        {
            // Handle pipeline flush events to reset VAD state
            if (controlFrame->command == transport::ControlCommand::INTERRUPT) {
                spdlog::debug("[VADFrameProcessor] Received INTERRUPT control frame, resetting VAD state to QUIET.");
                switchToState(transport::VADState::ENDED);
                size_t offset = _ringBuffer ? _ringBuffer->getOffset() : 0;
                emitStateFrame(offset, offset, 0.0f); // Emit state frame with 0 offset and 0 confidence on flush
                switchToState(transport::VADState::QUIET);
                emitStateFrame(offset, offset, 0.0f); // Emit state frame with 0 offset and 0 confidence on flush
            }
        }

        void VADFrameProcessor::processQuiet(const va::audio::VADProvider::VADResult* vadResult)
        {
            if (vadResult) {
                spdlog::debug("[VADFrameProcessor] VAD detected voice activity in QUIET state, preparing to switch to STARTING...");
                switchToState(va::transport::VADState::STARTING);
                _isInSpeech = true;

                // Speech offsets are absolute; VAD result offsets are relative to the current audio buffer
                int lookbackSamples = static_cast<int>(_lookbackMultiplier * (_absoluteAudioOffset - _absoluteAudioPreOffset));
                // Signed 64-bit: looking back can go negative relative to the current audio offset
                int64_t speechStartOffset = static_cast<int64_t>(_absoluteAudioOffset) + vadResult->offset - lookbackSamples;

                _absoluteSpeechOffsets.start = _ringBuffer->normalizeOffset(speechStartOffset);
                _absoluteSpeechOffsets.end = _ringBuffer->normalizeOffset(_absoluteSpeechOffsets.start + vadResult->length);
                emitStateFrame(_absoluteSpeechOffsets.start, _absoluteSpeechOffsets.end, vadResult->confidence);
            }
            else {
                // skip
            }
        }

        void VADFrameProcessor::processStarting(const va::audio::VADProvider::VADResult* vadResult)
        {
            size_t audioProgress = _absoluteAudioOffset - _absoluteSpeechOffsets.start;

            const uint64_t minSpeechDurationSamples = static_cast<uint64_t>(_vadConfig.minSpeechDurationMs * _vadProvider->getAudioConfig().sampleRate / 1000);

            if (vadResult) {
                // Update end offset as more VAD results arrive, so ONGOING reflects accurate speech start time
                size_t previousEnd = _absoluteSpeechOffsets.end;
                _absoluteSpeechOffsets.end += vadResult->offset + vadResult->length;

                size_t currentSpeechLength = _absoluteSpeechOffsets.end - _absoluteSpeechOffsets.start;

                if (audioProgress >= minSpeechDurationSamples) {
                    if (currentSpeechLength >= minSpeechDurationSamples) {
                        switchToState(transport::VADState::ONGOING);
                        emitStateFrame(previousEnd, _absoluteSpeechOffsets.end, vadResult->confidence);
                    }
                    else {
                        switchToState(transport::VADState::QUIET);
                        emitStateFrame(_absoluteSpeechOffsets.end, _absoluteSpeechOffsets.end, vadResult->confidence);
                    }
                }
            }
            else {
                spdlog::debug("[VADFrameProcessor] No voice activity detected in STARTING state. Audio progress since potential speech start: {} samples", audioProgress);
                if (audioProgress >= minSpeechDurationSamples) {
                    switchToState(transport::VADState::QUIET);
                    emitStateFrame(_absoluteSpeechOffsets.end, _absoluteSpeechOffsets.end, 0.0f);
                }
                else {
                    spdlog::debug("[VADFrameProcessor] Minimum speech duration not yet met in STARTING state, remaining in STARTING.");
                }
            }
        }

        void VADFrameProcessor::processOngoing(const va::audio::VADProvider::VADResult* vadResult)
        {
            if (vadResult != nullptr) {
                size_t previousEnd = _absoluteSpeechOffsets.end;
                // Update end offset with the newest VAD result to track speech progress
                _absoluteSpeechOffsets.end += vadResult->offset + vadResult->length;

                const uint64_t minSpeechDurationSamples = static_cast<uint64_t>(_vadConfig.minSpeechDurationMs * _vadProvider->getAudioConfig().sampleRate / 1000);
                size_t currentSpeechLength = _absoluteSpeechOffsets.end - _absoluteSpeechOffsets.start;
                size_t audioProgress = _absoluteAudioOffset - _absoluteSpeechOffsets.end;

                emitStateFrame(previousEnd, _absoluteSpeechOffsets.end, vadResult->confidence);
            }
            else {
                spdlog::debug("[VADFrameProcessor] No voice activity detected in ONGOING state.");
                switchToState(transport::VADState::PAUSED);
                // End offset = current audio offset, marking the pause point
                emitStateFrame(_absoluteSpeechOffsets.end, _absoluteAudioOffset, 0.0f);
            }
        }

        void VADFrameProcessor::processPaused(const va::audio::VADProvider::VADResult* vadResult)
        {
            if (vadResult) {
                spdlog::debug("[VADFrameProcessor] Voice activity detected in PAUSED state, preparing to switch to RESUMING...");
                switchToState(transport::VADState::RESUMING);
                int previousEnd = _absoluteSpeechOffsets.end;
                _absoluteSpeechOffsets.end += vadResult->offset + vadResult->length;
                emitStateFrame(previousEnd, _absoluteSpeechOffsets.end, vadResult->confidence);
            }
            else {
                size_t maxSpeechPauseSamples = static_cast<uint64_t>(_vadConfig.maxSpeechPauseDurationMs * _vadProvider->getAudioConfig().sampleRate / 1000);
                if (_absoluteAudioOffset - _stateEntryAudioOffset >= maxSpeechPauseSamples) {
                    spdlog::debug("[VADFrameProcessor] No voice activity detected for {} ms in PAUSED state, switching to ENDING...", _vadConfig.maxSpeechPauseDurationMs);
                    switchToState(transport::VADState::ENDING);
                    emitStateFrame(_absoluteSpeechOffsets.end, _absoluteAudioOffset, 0.0f);
                }
            }
        }

        void VADFrameProcessor::processResuming(const va::audio::VADProvider::VADResult* vadResult)
        {
            if (vadResult) {
                spdlog::debug("[VADFrameProcessor] Voice activity detected in RESUMING state, preparing to switch back to ONGOING...");
                size_t minSpeechDurationSamples = static_cast<size_t>(_vadConfig.minSpeechDurationMs * _vadProvider->getAudioConfig().sampleRate / 1000);
                size_t audioProgressSinceResume = _absoluteAudioOffset - _stateEntryAudioOffset;

                if (audioProgressSinceResume >= minSpeechDurationSamples) {
                    switchToState(transport::VADState::ONGOING);
                    size_t previousEnd = _absoluteSpeechOffsets.end;
                    _absoluteSpeechOffsets.end += vadResult->offset + vadResult->length;
                    emitStateFrame(previousEnd, _absoluteSpeechOffsets.end, vadResult->confidence);
                }
                else {
                    spdlog::debug("[VADFrameProcessor] Minimum speech duration not yet met in RESUMING state, remaining in RESUMING.");
                }
            }
            else {
                spdlog::debug("[VADFrameProcessor] No voice activity detected in RESUMING state.");
                switchToState(transport::VADState::PAUSED);
                emitStateFrame(_absoluteSpeechOffsets.end, _absoluteSpeechOffsets.end, 0.0f);
            }
        }

        void VADFrameProcessor::processEnding(const va::audio::VADProvider::VADResult* vadResult)
        {
            if (vadResult) {
                spdlog::debug("[VADFrameProcessor] Voice activity detected in ENDING state, preparing to switch to RESUMING...");
                switchToState(transport::VADState::RESUMING);
                size_t previousEnd = _absoluteSpeechOffsets.end;
                _absoluteSpeechOffsets.end += vadResult->offset + vadResult->length;
                emitStateFrame(previousEnd, _absoluteSpeechOffsets.end, vadResult->confidence);
            }
            else {
                spdlog::debug("[VADFrameProcessor] No voice activity detected in ENDING state.");
                size_t maxSpeechEndSamples = static_cast<uint64_t>(_vadConfig.maxSpeechEndDurationMs * _vadProvider->getAudioConfig().sampleRate / 1000);
                if (_absoluteAudioOffset - _stateEntryAudioOffset >= maxSpeechEndSamples) {
                    switchToState(transport::VADState::ENDED);
                    emitStateFrame(_absoluteSpeechOffsets.start, _absoluteAudioOffset, 0.0f);
                    
                    switchToState(transport::VADState::QUIET);
                    emitStateFrame(_absoluteAudioOffset, _absoluteAudioOffset, 0.0f);
                }
            }
        }

        void VADFrameProcessor::processEnded(const va::audio::VADProvider::VADResult* vadResult)
        {
            // skipped
            switchToState(transport::VADState::QUIET);
            emitStateFrame(_absoluteSpeechOffsets.end, _absoluteSpeechOffsets.end, 0.0f);
        }

        std::vector<audio::VADProvider::VADResult> VADFrameProcessor::mergeVadResults(const std::vector<audio::VADProvider::VADResult>& vadResults,
            int pauseWindowLengthSamples) const
        {
            std::vector<va::audio::VADProvider::VADResult> mergedVadResults;
            if (vadResults.empty()) return mergedVadResults;

            mergedVadResults.reserve(vadResults.size());

            for (const auto& r : vadResults) {
                if (!mergedVadResults.empty()) {
                    auto& last = mergedVadResults.back();
                    // if the current VAD result is close enough to the previous one, merge them into a single result
                    if (r.offset <= last.offset + last.length + pauseWindowLengthSamples) {
                        last.length = std::max(last.length, r.offset + r.length - last.offset);
                        continue;
                    }
                }
                mergedVadResults.emplace_back(r);
            }

            // Convert offsets to be relative to the end of the previous result.
            // result[0] stays batch-absolute; result[i] (i>0) becomes the gap
            // between result[i-1]'s end and result[i]'s start.
            // The processing loop advances _absoluteAudioPreOffset by each
            // result's (offset + length) so that the next handler's base is correct.
            for (int i = (int)mergedVadResults.size() - 1; i > 0; --i) {
                mergedVadResults[i].offset -= mergedVadResults[i - 1].offset + mergedVadResults[i - 1].length;
            }

            return mergedVadResults;
        }

        void VADFrameProcessor::switchToState(transport::VADState newState) {
            spdlog::info("[VADFrameProcessor] Switching VAD state from {} to {}", va::transport::toString(_currentState), va::transport::toString(newState));
            _previousState = _currentState;
            _currentState = newState;
            _stateEntryAudioOffset = _absoluteAudioOffset;
        }

        void VADFrameProcessor::emitStateFrame(uint64_t speechStartOffset, uint64_t speechEndOffset, float confidence) {
            auto vadFrame = std::make_shared<transport::VADStateFrame>();
            vadFrame->state = _currentState;
            vadFrame->speechStartOffset = _ringBuffer->normalizeOffset(speechStartOffset);
            vadFrame->speechEndOffset = _ringBuffer->normalizeOffset(speechEndOffset);
            vadFrame->confidence = confidence;
            vadFrame->totalSpeechLength = _ringBuffer->computeDeltaOffset(vadFrame->speechStartOffset, vadFrame->speechEndOffset);
            vadFrame->timestamp = transport::Frame::getCurrentTimestamp();
            vadFrame->sourceId = getName();
            pushDownstream(vadFrame);

            // Observe completed speech segment duration when speech ends.
            if (_currentState == transport::VADState::ENDED && vadFrame->totalSpeechLength > 0) {
                // totalSpeechLength is in samples; convert to ms using the provider's sample rate.
                auto sampleRate = _vadProvider ? _vadProvider->getAudioConfig().sampleRate : 16000;
                double durationMs = (static_cast<double>(vadFrame->totalSpeechLength) / sampleRate) * 1000.0;
                _metrics.speechDur.observe(durationMs);
                _metrics.segments.increment();
            }
        }

        void VADFrameProcessor::processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) {
            if (direction == FlowDirection::DOWNSTREAM) {
                switch (frame->getType()) {
                case transport::FrameType::AUDIO: {
                    auto audioFrame = std::dynamic_pointer_cast<transport::AudioFrame>(frame);
                    if (audioFrame && _vadProvider && _ringBuffer) {
                        processAudioFrame(audioFrame);
                    }
                } break;
                case transport::FrameType::CONTROL: {
                    auto controlFrame = std::dynamic_pointer_cast<transport::ControlFrame>(frame);
                    if (controlFrame) {
                        processControlFrame(controlFrame);
                    }
                } break;
                }
                pushDownstream(frame);
            }
        }
    } // namespace pipeline
} // namespace va
