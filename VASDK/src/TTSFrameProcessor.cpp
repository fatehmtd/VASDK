#include "VASDK/TTSFrameProcessor.h"
#include "VASDK/nlu/English.h"
#include <VASDK/profiling/CTrack.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <memory>
#include <chrono>
#include <vector>

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
            {
                std::lock_guard<std::mutex> lock(_holdReleaseMutex);
                _holdReleaseStop = false;
                _holdReleaseScheduled = false;
            }
            if (!_holdReleaseThread.joinable())
                _holdReleaseThread = std::thread(&TTSFrameProcessor::holdReleaseLoop, this);
            return _synthesisProvider->startVoiceSynthesisProvider();
        }

        void TTSFrameProcessor::stop() {
            {
                std::lock_guard<std::mutex> lock(_holdReleaseMutex);
                _holdReleaseStop = true;
            }
            _holdReleaseCv.notify_all();
            if (_holdReleaseThread.joinable()) _holdReleaseThread.join();
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
                {
                    std::lock_guard<std::mutex> lock(_turnMutex);
                    _pendingSentences.clear();
                    _providerBusy = _textClosed = _turnActive = _sentInTurn = _protectedSending = false;
                    _protectedStart = _protectedEnd = {};
                    _playbackPaused = false;  // the queued audio is gone
                }
                if (_playbackFlush) _playbackFlush(); // flush SDL buffer immediately
                if (_synthesisProvider) _synthesisProvider->cancelSynthesis();
                emitSynthesisState(transport::SynthesisState::INTERRUPTED);
                break;
            case transport::AgentStateTransition::INTERRUPT_RESOLVED:
                spdlog::debug("[TTSFrameProcessor] Interrupt resolved - clearing cancelPending");
                _cancelPending = false;
                holdPlayback(false);
                break;
            case transport::AgentStateTransition::BARGE_IN_PENDING:
                holdPlayback(true);
                break;
            case transport::AgentStateTransition::BACKCHANNEL:
                releaseDuckAfterMinimumDuration();
                break;
            case transport::AgentStateTransition::SYNTHESIS_COMPLETED:
                holdPlayback(false);
                break;
            default:
                break;
            }
        }

        void TTSFrameProcessor::holdPlayback(bool held) {
            if (_pendingPlaybackMode == transport::PendingPlaybackMode::Continue) return;
            if (_pendingPlaybackMode == transport::PendingPlaybackMode::Pause) {
                std::lock_guard<std::mutex> lock(_turnMutex);
                const auto now = std::chrono::steady_clock::now();
                if (held && !_playbackPaused) {
                    _playbackPaused = true;
                    _playbackRemainingWhilePaused = std::max(std::chrono::microseconds(0),
                        std::chrono::duration_cast<std::chrono::microseconds>(_playbackEndTime - now));
                } else if (!held && _playbackPaused) {
                    _playbackPaused = false;
                    _playbackEndTime = now + _playbackRemainingWhilePaused;
                }
            }
            std::lock_guard<std::mutex> lock(_holdReleaseMutex);
            _holdReleaseScheduled = false;  // a new hold, or an immediate release, supersedes a pending one
            if (held) _holdStartTime = std::chrono::steady_clock::now();
            if (_playbackHold) _playbackHold(_pendingPlaybackMode, held, _duckGainDecibels);
        }

        void TTSFrameProcessor::releaseDuckAfterMinimumDuration() {
            if (_pendingPlaybackMode != transport::PendingPlaybackMode::Duck) {
                holdPlayback(false);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(_holdReleaseMutex);
                const auto releaseTime = _holdStartTime + std::chrono::milliseconds(_duckDurationMs);
                if (releaseTime > std::chrono::steady_clock::now()) {
                    _holdReleaseTime = releaseTime;
                    _holdReleaseScheduled = true;
                    _holdReleaseCv.notify_all();
                    return;
                }
            }
            holdPlayback(false);
        }

        void TTSFrameProcessor::holdReleaseLoop() {
            std::unique_lock<std::mutex> lock(_holdReleaseMutex);
            while (!_holdReleaseStop) {
                if (!_holdReleaseScheduled) {
                    _holdReleaseCv.wait(lock, [this] { return _holdReleaseStop || _holdReleaseScheduled; });
                    continue;
                }
                const auto releaseTime = _holdReleaseTime;
                if (_holdReleaseCv.wait_until(lock, releaseTime, [this, releaseTime] {
                        return _holdReleaseStop || !_holdReleaseScheduled || _holdReleaseTime != releaseTime;
                    }))
                    continue;  // stopped, superseded or rescheduled
                _holdReleaseScheduled = false;
                if (_playbackHold) _playbackHold(_pendingPlaybackMode, false, _duckGainDecibels);
            }
        }

        int TTSFrameProcessor::getRemainingReplyPlaybackMs() {
            std::lock_guard<std::mutex> lock(_turnMutex);
            // Until every sentence is synthesized the reply's length isn't known; 0 would read as "almost done".
            if (!_turnActive || !_textClosed || !_draining) return -1;
            const auto remaining = _playbackPaused
                ? _playbackRemainingWhilePaused
                : std::chrono::duration_cast<std::chrono::microseconds>(_playbackEndTime - std::chrono::steady_clock::now());
            return static_cast<int>(std::max<int64_t>(0, remaining.count() / 1000));
        }

        int TTSFrameProcessor::getProtectedPlaybackMs() {
            std::lock_guard<std::mutex> lock(_turnMutex);
            const auto now = std::chrono::steady_clock::now();
            // The sink plays a little behind the model: the hardware buffer.
            const auto margin = std::chrono::milliseconds(_hardwareBufferMarginMs);
            if (_protectedStart == std::chrono::steady_clock::time_point{} || now < _protectedStart + margin) return 0;
            const auto end = _protectedEnd + margin;
            if (now >= end) return _protectedSending ? 1 : 0;   // still synthesizing: more is coming
            return std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - now).count()));
        }

        void TTSFrameProcessor::synthesizeTextFrame(std::shared_ptr<transport::TextFrame> frame) {
            if (!frame || !_synthesisProvider) return;

            std::string choicesStr;
            for (const auto& choice : frame->choices) {
                choicesStr += choice.text + "; ";
            }
            spdlog::info("[TTSFrameProcessor] Received Structured Text frame: '{}', isFinal={}, isExpecting={}, choices={}",
                frame->text, frame->isFinal, frame->isExpecting, choicesStr);

            bool mergeIntoDrainingTurn = false;
            {
                std::lock_guard<std::mutex> lock(_turnMutex);
                if (_textClosed) {
                    // A new reply while the previous one is still playing out: one continuous turn.
                    _textClosed = false;
                    mergeIntoDrainingTurn = _draining;
                }
                const bool ssml = !frame->ssml.empty() && _synthesisProvider->supportsSsml();
                if (ssml || !frame->text.empty())
                    _pendingSentences.push_back({ssml                                   ? frame->ssml
                                                 : _synthesisProvider->normalizesText() ? frame->text
                                                                                        : nlu::english::speakable(frame->text),
                                                 frame->interruptible});
                if (frame->isFinal) _textClosed = true;
            }
            if (mergeIntoDrainingTurn) cancelDrainWait();
            pumpSentences();
            maybeStartDrain();
        }

        void TTSFrameProcessor::pumpSentences() {
            std::vector<std::string> toSend;
            {
                std::lock_guard<std::mutex> lock(_turnMutex);
                const bool queued = _synthesisProvider->acceptsQueuedSynthesis();
                while (!_pendingSentences.empty() && (queued || !_providerBusy)) {
                    const bool isProtected = !_pendingSentences.front().interruptible;
                    // A part that can't be interrupted is synthesized alone, so its audio is all its own.
                    if ((isProtected || _protectedSending) && _providerBusy) break;
                    const auto now = std::chrono::steady_clock::now();
                    if (!_sentInTurn) {
                        _sentInTurn = true;
                        _synthesisStartTime = now;
                    }
                    if (isProtected) {
                        _protectedSending = true;
                        _protectedStart = _protectedEnd = _turnActive ? std::max(now, _playbackEndTime) : now;
                    }
                    toSend.push_back(std::move(_pendingSentences.front().text));
                    _pendingSentences.pop_front();
                    _providerBusy = true;
                }
            }
            // Outside the lock: cloud providers fire onSynthesisStarted() from inside this call.
            for (const auto& text : toSend) {
                spdlog::info("[TTSFrameProcessor] Synthesizing: {}", text);
                _metrics.requests.increment();
                _synthesisProvider->synthesizeSpeech(text);
            }
        }

        void TTSFrameProcessor::maybeStartDrain() {
            {
                std::lock_guard<std::mutex> lock(_turnMutex);
                if (!_textClosed || !_pendingSentences.empty() || _providerBusy || _draining) return;
                if (!_turnActive) {
                    // The reply closed without any audio (empty text): nothing to complete.
                    _textClosed = _sentInTurn = false;
                    return;
                }
                _draining = true;
            }
            spdlog::info("[TTSFrameProcessor] Synthesis completed - waiting for audio drain");
            startDrainWait();
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

        // VoiceSynthesisResultListener implementation. Provider started/completed only say
        // whether the provider is busy; the turn's STARTED/COMPLETED are decided here.
        void TTSFrameProcessor::onSynthesisStarted() {
            bool emitStarted = false, resumeFromDrain = false;
            {
                std::lock_guard<std::mutex> lock(_turnMutex);
                if (_cancelPending) return;
                _providerBusy = true;
                resumeFromDrain = _draining;  // a queued sentence started after the provider idled
                if (!_turnActive) {
                    _turnActive = true;
                    _firstChunkReceived = false;
                    _playbackEndTime = std::chrono::steady_clock::now();
                    emitStarted = true;
                }
            }
            if (resumeFromDrain) cancelDrainWait();
            if (emitStarted) {
                spdlog::info("[TTSFrameProcessor] Synthesis started");
                emitSynthesisState(transport::SynthesisState::STARTED);
            }
        }

        void TTSFrameProcessor::onSynthesisCompleted() {
            {
                std::lock_guard<std::mutex> lock(_turnMutex);
                if (_cancelPending) {
                    // processFrame already flushed SDL and emitted INTERRUPTED.
                    spdlog::info("[TTSFrameProcessor] Synthesis finished after interrupt (no-op)");
                    return;
                }
                _providerBusy = _protectedSending = false;
            }
            pumpSentences();
            maybeStartDrain();
        }

        void TTSFrameProcessor::onSynthesisError(const std::string& error) {
            spdlog::error("[TTSFrameProcessor] Synthesis error: {}", error);
            cancelDrainWait();
            {
                std::lock_guard<std::mutex> lock(_turnMutex);
                _pendingSentences.clear();
                _providerBusy = _textClosed = _turnActive = _sentInTurn = _protectedSending = false;
                _protectedStart = _protectedEnd = {};
            }
            if (_playbackFlush) _playbackFlush();
            emitSynthesisState(transport::SynthesisState::ERROR, error);
        }

        void TTSFrameProcessor::startDrainWait() {
            std::lock_guard<std::mutex> threadLock(_drainThreadMutex);
            {
                std::lock_guard<std::mutex> lock(_drainMutex);
                if (_drainThread.joinable()) _drainStop = true;
            }
            _drainCv.notify_all();
            if (_drainThread.joinable()) _drainThread.join();
            {
                std::lock_guard<std::mutex> lock(_drainMutex);
                _drainStop = false;
            }
            _drainThread = std::thread(&TTSFrameProcessor::drainWaitLoop, this);
        }

        void TTSFrameProcessor::cancelDrainWait() {
            std::lock_guard<std::mutex> threadLock(_drainThreadMutex);
            {
                std::lock_guard<std::mutex> lock(_drainMutex);
                _drainStop = true;
            }
            _drainCv.notify_all();
            if (_drainThread.joinable()) _drainThread.join();
            std::lock_guard<std::mutex> lock(_turnMutex);
            _draining = false;
        }

        void TTSFrameProcessor::drainWaitLoop() {
            // Wait until everything handed to the sink has played. The end time is re-read on every
            // pass because late chunks of the turn keep extending it.
            while (true) {
                int64_t remainingMs;
                {
                    std::lock_guard<std::mutex> lock(_turnMutex);
                    remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        _playbackEndTime - std::chrono::steady_clock::now()).count()
                        + static_cast<int64_t>(_hardwareBufferMarginMs);
                    if (_playbackPaused) remainingMs = 100;  // paused audio hasn't played; keep waiting
                }
                if (remainingMs <= 0) break;
                std::unique_lock<std::mutex> lock(_drainMutex);
                if (_drainCv.wait_for(lock, std::chrono::milliseconds(std::min<int64_t>(remainingMs, 100)),
                                      [this] { return _drainStop; }))
                    return; // cancelled by interrupt or by the turn resuming
            }

            {
                std::lock_guard<std::mutex> lock(_turnMutex);
                _turnActive = _textClosed = _sentInTurn = _draining = false;
            }
            spdlog::info("[TTSFrameProcessor] Audio drained - emitting COMPLETED");
            if (_onAudioDrained) _onAudioDrained();
            emitSynthesisState(transport::SynthesisState::COMPLETED);
        }

        void TTSFrameProcessor::onAudioDataReceived(const char* data, size_t size) {
            if (_cancelPending) return; // Barge-in in progress - discard incoming audio

            auto now = std::chrono::steady_clock::now();
            auto audioConfig = _synthesisProvider->getAudioConfiguration();
            int bytesPerSample = audioConfig.getSampleSizeInBytes();
            int channels = audioConfig.channels;
            int sampleRate = audioConfig.sampleRate;
            int64_t chunkUs = 0;
            if (bytesPerSample > 0 && channels > 0 && sampleRate > 0) {
                chunkUs = (static_cast<int64_t>(size) / (bytesPerSample * channels)) * 1000000 / sampleRate;
            }
            {
                std::lock_guard<std::mutex> lock(_turnMutex);
                if (!_firstChunkReceived) {
                    _firstChunkReceived = true;
                    _metrics.ttfa.observe(std::chrono::duration<double, std::milli>(
                        now - _synthesisStartTime).count());
                }
                _metrics.chunk.observe(std::chrono::duration<double, std::milli>(
                    now - _synthesisStartTime).count());
                // Playback-end model for the drain wait (see _playbackEndTime).
                if (_playbackPaused)
                    _playbackRemainingWhilePaused += std::chrono::microseconds(chunkUs);
                else
                    _playbackEndTime = std::max(_playbackEndTime, now) + std::chrono::microseconds(chunkUs);
                if (_protectedSending) _protectedEnd = _playbackEndTime;
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
