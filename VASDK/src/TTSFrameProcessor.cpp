#include "VASDK/TTSFrameProcessor.h"
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>
#include <memory>
#include <chrono>

namespace va {
    namespace pipeline {

        TTSFrameProcessor::TTSFrameProcessor(
            std::shared_ptr<audio::GenericVoiceSynthesisProvider> provider)
            : _synthesisProvider(provider) {
        }

        TTSFrameProcessor::~TTSFrameProcessor() {
            stop();
        }

        bool TTSFrameProcessor::initialize() {
            if (!FrameProcessor::initialize()) return false;
            if (!_synthesisProvider) {
                spdlog::error("[TTSFrameProcessor] No synthesis provider");
                return false;
            }
            _synthesisProvider->setVoiceSynthesisResultListener(this);

            // Pre-register metrics
            if (_metrics.config.enabled && _collector) {
                using namespace va::metrics;
                _metrics.ttfa.initialize(_collector,
                    MetricDescriptor{"tts_ttfa_ms", MetricType::Sampler,
                                     "TTS time-to-first-audio (ms)",
                                     _metrics.config.ttfaWindow});
                _metrics.chunk.initialize(_collector,
                    MetricDescriptor{"tts_chunk_ms", MetricType::Sampler,
                                     "TTS inter-chunk delivery interval (ms)",
                                     _metrics.config.chunkWindow});
                _metrics.requests.initialize(_collector,
                    MetricDescriptor{"tts_requests_total", MetricType::Counter,
                                     "Total TTS synthesis requests"});
                if (_metrics.config.ttfaAlertMs > 0.0) {
                    _metrics.ttfaAlert.initialize(_collector,
                        MetricDescriptor{"tts_ttfa_alert_ms", MetricType::Gauge,
                                         "TTFA alert threshold (ms)"});
                    _metrics.ttfaAlert.set(_metrics.config.ttfaAlertMs);
                }
            }

            return true;
        }

        bool TTSFrameProcessor::start() {
            if (!FrameProcessor::start()) return false;
            return _synthesisProvider->startVoiceSynthesisProvider();
        }

        void TTSFrameProcessor::stop() {
            cancelDrainWait();
            if (_synthesisProvider) _synthesisProvider->stopVoiceSynthesisProvider();
            FrameProcessor::stop();
        }

        void TTSFrameProcessor::processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) {
            VASDK_CTRACK_NAME("TTS::processFrame");
            if (direction == FlowDirection::DOWNSTREAM) {
                if (frame->getType() == transport::FrameType::AGENT_STATE) {
                    handleAgentStateFrame(std::dynamic_pointer_cast<transport::AgentStateFrame>(frame));
                    pushDownstream(frame);
                    return;
                }

                if (_cancelPending) {
                    spdlog::debug("[TTSFrameProcessor] Dropping frame - cancel pending");
                }
                else if (frame->getType() == transport::FrameType::TEXT) {
                    // TranscriptionFrames are intentionally not synthesized - TTS speaks
                    // only LLM responses (TextFrames), not user transcriptions.
                    synthesizeTextFrame(std::dynamic_pointer_cast<transport::TextFrame>(frame));
                }
            }
            pushDownstream(frame);
        }

        void TTSFrameProcessor::handleAgentStateFrame(std::shared_ptr<transport::AgentStateFrame> frame) {
            if (!frame) return;

            switch (frame->transition) {
            case transport::AgentStateTransition::BARGE_IN_INTERRUPT:
            case transport::AgentStateTransition::USER_SPOKE_WHILE_THINKING:
                spdlog::info("[TTSFrameProcessor] Cancelling synthesis due to interrupt");
                _cancelPending = true;
                cancelDrainWait();              // stop drain wait if already running
                if (_playbackFlush) _playbackFlush(); // flush SDL buffer immediately
                if (_synthesisProvider) _synthesisProvider->cancelSynthesis();
                emitSynthesisState(transport::SynthesisState::INTERRUPTED);
                break;
            case transport::AgentStateTransition::INTERRUPT_RESOLVED:
                spdlog::debug("[TTSFrameProcessor] Interrupt resolved - clearing cancelPending");
                _cancelPending = false;
                break;
            default:
                break;
            }
        }

        void TTSFrameProcessor::synthesizeTextFrame(std::shared_ptr<transport::TextFrame> frame) {
            if (!frame) return;

            std::string choicesStr;
            for (const auto& choice : frame->choices) {
                choicesStr += choice.text + "; ";
            }
            spdlog::info("[TTSFrameProcessor] Received Structured Text frame: '{}', isFinal={}, isExpecting={}, choices={}",
                frame->text, frame->isFinal, frame->isExpecting, choicesStr);

            if (!frame->isFinal || frame->text.empty() || !_synthesisProvider) return;

            spdlog::info("[TTSFrameProcessor] Synthesizing: {}", frame->text);
            _metrics.requests.increment();
            _synthesisProvider->synthesizeSpeech(frame->text);
        }

        void TTSFrameProcessor::emitSynthesisState(transport::SynthesisState state, const std::string& errorMessage) {
            auto f = std::make_shared<transport::SynthesisStateFrame>();
            f->state = state;
            f->errorMessage = errorMessage;
            f->timestamp = transport::Frame::getCurrentTimestamp();
            f->sourceId = getName();
            pushUpstream(f);    // AgentStateProcessor, TurnDetectionProcessor react upstream
            pushDownstream(f);  // downstream observers (TUI, SynthesisObserver, etc.)
        }

        // VoiceSynthesisResultListener implementation
        void TTSFrameProcessor::onSynthesisStarted() {
            spdlog::info("[TTSFrameProcessor] Synthesis started");
            _cancelPending = false;
            _firstChunkReceived = false;
            _synthesisStartTime = std::chrono::steady_clock::now();
            _playbackStartTime  = _synthesisStartTime; // will be overwritten on first audio chunk
            _totalAudioDurationMs.store(0);
            emitSynthesisState(transport::SynthesisState::STARTED);
        }

        void TTSFrameProcessor::onSynthesisCompleted() {
            if (_cancelPending) {
                // processFrame already flushed SDL and emitted INTERRUPTED - just reset the flag.
                spdlog::info("[TTSFrameProcessor] Synthesis finished after interrupt (no-op)");
                _cancelPending = false;
            }
            else {
                spdlog::info("[TTSFrameProcessor] Synthesis completed - waiting for audio drain");
                startDrainWait();
            }
        }

        void TTSFrameProcessor::onSynthesisError(const std::string& error) {
            spdlog::error("[TTSFrameProcessor] Synthesis error: {}", error);
            _cancelPending = false;
            cancelDrainWait();
            if (_playbackFlush) _playbackFlush();
            emitSynthesisState(transport::SynthesisState::ERROR, error);
        }

        void TTSFrameProcessor::startDrainWait() {
            cancelDrainWait();
            {
                std::lock_guard<std::mutex> lock(_drainMutex);
                _drainStop = false;
            }
            _drainThread = std::thread(&TTSFrameProcessor::drainWaitLoop, this);
        }

        void TTSFrameProcessor::cancelDrainWait() {
            {
                std::lock_guard<std::mutex> lock(_drainMutex);
                _drainStop = true;
            }
            _drainCv.notify_all();
            if (_drainThread.joinable()) _drainThread.join();
        }

        void TTSFrameProcessor::drainWaitLoop() {
            // Elapsed time since the FIRST audio chunk was handed to the sink.
            // Audio starts playing at that point, so this correctly measures how
            // much of the queued audio has already been consumed by the speaker.
            // Using _synthesisStartTime (before ONNX inference) would overcount
            // elapsed time and cause COMPLETED to fire before playback finishes.
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - _playbackStartTime).count();
            auto total = _totalAudioDurationMs.load() + static_cast<int64_t>(_hardwareBufferMarginMs);
            auto remainingMs = total - elapsed;

            spdlog::debug("[TTSFrameProcessor] Drain wait: total={}ms, elapsed={}ms, remaining={}ms",
                          total, elapsed, remainingMs);

            if (remainingMs > 0) {
                std::unique_lock<std::mutex> lock(_drainMutex);
                _drainCv.wait_for(lock, std::chrono::milliseconds(remainingMs),
                                  [this] { return _drainStop; });
            }

            if (_drainStop) return; // cancelled by interrupt

            spdlog::info("[TTSFrameProcessor] Audio drained - emitting COMPLETED");
            if (_onAudioDrained) _onAudioDrained();
            emitSynthesisState(transport::SynthesisState::COMPLETED);
        }

        void TTSFrameProcessor::onAudioDataReceived(const char* data, size_t size) {
            if (_cancelPending) return; // Barge-in in progress - discard incoming audio

            auto now = std::chrono::steady_clock::now();

            if (!_firstChunkReceived) {
                _firstChunkReceived = true;
                _playbackStartTime  = now; // audio starts entering the sink here
                double ms = std::chrono::duration<double, std::milli>(
                    now - _synthesisStartTime).count();
                _metrics.ttfa.observe(ms);
            }
            double ms = std::chrono::duration<double, std::milli>(
                now - _synthesisStartTime).count();
            _metrics.chunk.observe(ms);

            auto audioConfig = _synthesisProvider->getAudioConfiguration();

            // Accumulate playback duration for drain tracking
            int bytesPerSample = audioConfig.getSampleSizeInBytes();
            int channels = audioConfig.channels;
            int sampleRate = audioConfig.sampleRate;
            if (bytesPerSample > 0 && channels > 0 && sampleRate > 0) {
                int64_t chunkMs = (static_cast<int64_t>(size) / (bytesPerSample * channels))
                                  * 1000 / sampleRate;
                _totalAudioDurationMs.fetch_add(chunkMs);
            }

            auto audioFrame = std::make_shared<transport::SynthesisFrame>();
            audioFrame->audioData.reserve(size);
            audioFrame->audioData.insert(audioFrame->audioData.end(), data, data + size);
            audioFrame->timestamp = transport::Frame::getCurrentTimestamp();
            audioFrame->sourceId = getName();
            pushDownstream(audioFrame);
        }

        bool TTSFrameProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const {
            auto type = frame->getType();
            return type == transport::FrameType::TEXT
                || type == transport::FrameType::TRANSCRIPTION
                || type == transport::FrameType::AGENT_STATE;
        }

        void TTSFrameProcessor::setMetricsConfig(const va::metrics::TTSMetricsConfig& config)
        {
            _metrics.config = config;
            FrameProcessor::setMetricsConfig(static_cast<const va::metrics::ProcessorMetricsConfig&>(config));
        }

    } // namespace pipeline
} // namespace va
