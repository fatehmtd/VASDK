#include "VASDK/QueuedVoiceSynthesisProvider.h"

#include "VASDK/AudioSIMD.h"
#include "VASDK/profiling/CTrack.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <exception>

namespace va {
    namespace audio {

        // ---------------------------------------------------------------------------
        // StreamResampler
        // ---------------------------------------------------------------------------

        double StreamResampler::weight(double x) const {
            const double pi = 3.14159265358979323846;
            auto sinc = [pi](double v) {
                if (std::abs(v) < 1e-9) return 1.0;
                const double pv = pi * v;
                return std::sin(pv) / pv;
            };
            return std::abs(x) < _lanczosA ? sinc(x) * sinc(x / _lanczosA) : 0.0;
        }

        void StreamResampler::reset(int srcRate, int dstRate) {
            _src = srcRate;
            _dst = dstRate;
            _buf.clear();
            _bufStart = 0;
            _nextOut  = 0;
            if (passthrough()) return;
            const double ratio = static_cast<double>(dstRate) / srcRate;
            _step        = static_cast<double>(srcRate) / dstRate;
            _filterScale = std::min(1.0, ratio);
            _kernelR     = static_cast<int>(std::ceil(_lanczosA / _filterScale));
        }

        void StreamResampler::produce(std::vector<float>& out, int64_t inputEnd) {
            while (true) {
                const double  center = _nextOut * _step;
                const int64_t c0     = static_cast<int64_t>(std::floor(center));
                if (c0 + _kernelR >= inputEnd) break;   // needs the kernel's whole right half
                double sample = 0.0, wSum = 0.0;
                for (int64_t j = c0 - _kernelR + 1; j <= c0 + _kernelR; ++j) {
                    if (j < 0) continue;                // before the stream's start
                    const double w = weight((center - j) * _filterScale);
                    sample += _buf[static_cast<size_t>(j - _bufStart)] * w;
                    wSum   += w;
                }
                out.push_back(wSum > 0.0 ? static_cast<float>(sample / wSum) : 0.0f);
                ++_nextOut;
            }
            // Drop the input the next output sample's kernel no longer reaches.
            const int64_t keepFrom = static_cast<int64_t>(std::floor(_nextOut * _step)) - _kernelR + 1;
            if (keepFrom > _bufStart) {
                const int64_t drop = std::min<int64_t>(keepFrom - _bufStart, static_cast<int64_t>(_buf.size()));
                _buf.erase(_buf.begin(), _buf.begin() + drop);
                _bufStart += drop;
            }
        }

        void StreamResampler::process(const float* in, size_t n, std::vector<float>& out) {
            if (passthrough()) { out.insert(out.end(), in, in + n); return; }
            _buf.insert(_buf.end(), in, in + n);
            produce(out, _bufStart + static_cast<int64_t>(_buf.size()));
        }

        void StreamResampler::flush(std::vector<float>& out) {
            if (passthrough()) return;
            const int64_t totalIn  = _bufStart + static_cast<int64_t>(_buf.size());
            const int64_t totalOut = static_cast<int64_t>(std::ceil(totalIn * (static_cast<double>(_dst) / _src)));
            _buf.insert(_buf.end(), static_cast<size_t>(_kernelR) + 1, 0.0f);
            std::vector<float> tail;
            produce(tail, _bufStart + static_cast<int64_t>(_buf.size()));
            const int64_t keep = std::max<int64_t>(0, std::min<int64_t>(static_cast<int64_t>(tail.size()),
                totalOut - (_nextOut - static_cast<int64_t>(tail.size()))));
            out.insert(out.end(), tail.begin(), tail.begin() + keep);
        }

        // ---------------------------------------------------------------------------
        // QueuedVoiceSynthesisProvider: lifecycle
        // ---------------------------------------------------------------------------

        QueuedVoiceSynthesisProvider::QueuedVoiceSynthesisProvider(std::string logTag, Delivery delivery)
            : _logTag(std::move(logTag)), _delivery(delivery) {}

        QueuedVoiceSynthesisProvider::~QueuedVoiceSynthesisProvider() {
            // The provider stopped it already (its synthesizeOne() is gone by now); this only
            // keeps the threads from outliving the object if it didn't.
            stopVoiceSynthesisProvider();
        }

        bool QueuedVoiceSynthesisProvider::startVoiceSynthesisProvider() {
            if (_running) {
                spdlog::warn("[{}] Provider already running.", _logTag);
                return true;
            }
            if (!loadEngine()) return false;

            _stopWorker.store(false);
            _cancelSynth.store(false);
            _inSynthesisSequence = false;
            _deliveryDone = false;
            _running = true;

            _synthThread = std::thread(&QueuedVoiceSynthesisProvider::workerLoop, this);
            if (_delivery == Delivery::Thread)
                _deliveryThread = std::thread(&QueuedVoiceSynthesisProvider::deliveryLoop, this);

            spdlog::info("[{}] Provider started (pipeline: {} Hz {}ch)",
                _logTag, _audioConfig.sampleRate, _audioConfig.channels);
            return true;
        }

        void QueuedVoiceSynthesisProvider::stopVoiceSynthesisProvider() {
            if (!_running) return;

            cancelSynthesis();
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                _stopWorker.store(true);
            }
            _queueCv.notify_all();
            if (_synthThread.joinable()) _synthThread.join();

            if (_delivery == Delivery::Thread) {
                flushDeliverySlot();
                {
                    std::lock_guard<std::mutex> lock(_deliveryMutex);
                    _deliveryDone = true;
                }
                _deliveryReadyCv.notify_all();
                if (_deliveryThread.joinable()) _deliveryThread.join();
            }

            _running = false;
            spdlog::info("[{}] Provider stopped.", _logTag);
        }

        bool QueuedVoiceSynthesisProvider::isVoiceSynthesisProviderRunning() const {
            return _running;
        }

        // ---------------------------------------------------------------------------
        // The queue
        // ---------------------------------------------------------------------------

        bool QueuedVoiceSynthesisProvider::synthesizeSpeech(const std::string& text) {
            if (!_running) {
                spdlog::error("[{}] synthesizeSpeech() called but the provider is not running.", _logTag);
                return false;
            }
            if (text.empty()) return false;
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                _textQueue.push({text, _cancelGeneration});
            }
            _queueCv.notify_one();
            return true;
        }

        void QueuedVoiceSynthesisProvider::cancelSynthesis() {
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                while (!_textQueue.empty()) _textQueue.pop();
                ++_cancelGeneration;
                _cancelSynth.store(true);
            }
            onCancel();
            _queueCv.notify_all();
            if (_delivery == Delivery::Thread) flushDeliverySlot();
        }

        void QueuedVoiceSynthesisProvider::workerLoop() {
            while (true) {
                std::string text;
                bool isSequenceStart = false;
                {
                    std::unique_lock<std::mutex> lock(_queueMutex);
                    _queueCv.wait(lock, [this] { return !_textQueue.empty() || _stopWorker.load(); });
                    if (_stopWorker.load() && _textQueue.empty()) break;
                    if (_textQueue.empty()) continue;
                    auto queued = std::move(_textQueue.front());
                    _textQueue.pop();
                    if (queued.generation != _cancelGeneration) continue;   // queued before a cancel
                    text = std::move(queued.text);
                    // Queued after the cancel the sequence was stopped by: a new sequence, not part of it.
                    isSequenceStart = !_inSynthesisSequence || _cancelSynth.load();
                    _inSynthesisSequence = true;
                    if (isSequenceStart) _cancelSynth.store(false);
                }

                VASDK_CTRACK_NAME("TTS::processText");

                if (isSequenceStart && _resultListener)
                    _resultListener->onSynthesisStarted();

                _preBuffer.clear();
                _preBufferRate = 0;
                _preBufferFilled = false;

                bool completed = false;
                try {
                    completed = synthesizeOne(text);
                }
                catch (const std::exception& e) {
                    reportError(e.what());
                }

                bool shouldFireCompleted;
                {
                    std::lock_guard<std::mutex> lock(_queueMutex);
                    if (!completed || _cancelSynth.load()) {
                        // What's queued now came after the cancel (cancelSynthesis dropped the rest): it's said.
                        _inSynthesisSequence = false;
                        shouldFireCompleted = false;
                    }
                    else if (_textQueue.empty()) {
                        _inSynthesisSequence = false;   // all of the sequence was said
                        shouldFireCompleted = true;
                    }
                    else {
                        shouldFireCompleted = false;    // more of it to say
                    }
                }

                if (shouldFireCompleted) {
                    onSequenceCompleted();
                    if (_resultListener) _resultListener->onSynthesisCompleted();
                }
            }
        }

        void QueuedVoiceSynthesisProvider::reportError(const std::string& message) {
            spdlog::error("[{}] Synthesis failed: {}", _logTag, message);
            if (_resultListener) _resultListener->onSynthesisError(message);
        }

        // ---------------------------------------------------------------------------
        // Settings
        // ---------------------------------------------------------------------------

        void QueuedVoiceSynthesisProvider::setAudioConfiguration(const AudioConfig& config) {
            _audioConfig = config;
        }

        AudioConfig QueuedVoiceSynthesisProvider::getAudioConfiguration() const {
            return _audioConfig;
        }

        void QueuedVoiceSynthesisProvider::setVoiceSynthesisResultListener(VoiceSynthesisResultListener* listener) {
            _resultListener = listener;
        }

        VoiceSynthesisResultListener* QueuedVoiceSynthesisProvider::getVoiceSynthesisResultListener() const {
            return _resultListener;
        }

        void QueuedVoiceSynthesisProvider::setPreBufferedSamplesMS(int ms) {
            _preBufferedSamplesMS.store(std::max(0, ms));
        }

        int QueuedVoiceSynthesisProvider::getPreBufferedSamplesMS() const {
            return _preBufferedSamplesMS.load();
        }

        // ---------------------------------------------------------------------------
        // Delivery
        // ---------------------------------------------------------------------------

        void QueuedVoiceSynthesisProvider::handoffAudio(std::vector<float> pcm, int sampleRate) {
            if (pcm.empty() || _cancelSynth.load()) return;
            const int preBufferMS = _preBufferedSamplesMS.load();
            if (!_preBufferFilled && preBufferMS > 0) {
                // On a throttled CPU the first pieces come slower than they play: gather some
                // before playback starts.
                _preBuffer.insert(_preBuffer.end(), pcm.begin(), pcm.end());
                _preBufferRate = sampleRate;
                const size_t wanted = static_cast<size_t>(sampleRate) * static_cast<size_t>(preBufferMS) / 1000;
                if (_preBuffer.size() < wanted) return;
                _preBufferFilled = true;
                pcm = std::move(_preBuffer);
                _preBuffer.clear();
            }
            placeInSlot(std::move(pcm), sampleRate);
        }

        bool QueuedVoiceSynthesisProvider::finishHandoff() {
            // A short text can end before the pre-buffer is full.
            if (!_preBuffer.empty() && !_cancelSynth.load()) {
                std::vector<float> rest = std::move(_preBuffer);
                _preBuffer.clear();
                placeInSlot(std::move(rest), _preBufferRate);
            }
            _preBufferFilled = true;
            // onSynthesisCompleted() only once the delivery thread has all of the audio.
            std::unique_lock<std::mutex> lock(_deliveryMutex);
            _deliveryCv.wait(lock, [this] { return !_deliverySlot.has_value() || _cancelSynth.load(); });
            return !_cancelSynth.load();
        }

        void QueuedVoiceSynthesisProvider::placeInSlot(std::vector<float> pcm, int sampleRate) {
            std::unique_lock<std::mutex> lock(_deliveryMutex);
            _deliveryCv.wait(lock, [this] { return !_deliverySlot.has_value() || _cancelSynth.load(); });
            if (_cancelSynth.load()) return;   // barge-in: drop it
            _deliverySlot = Chunk{std::move(pcm), sampleRate};
            _deliveryReadyCv.notify_one();
        }

        void QueuedVoiceSynthesisProvider::flushDeliverySlot() {
            std::lock_guard<std::mutex> lock(_deliveryMutex);
            _deliverySlot.reset();
            _deliveryCv.notify_one();   // the worker may be waiting to hand over
        }

        void QueuedVoiceSynthesisProvider::deliveryLoop() {
            while (true) {
                Chunk chunk;
                {
                    std::unique_lock<std::mutex> lock(_deliveryMutex);
                    _deliveryReadyCv.wait(lock, [this] { return _deliverySlot.has_value() || _deliveryDone; });
                    if (_deliveryDone && !_deliverySlot.has_value()) break;
                    chunk = std::move(*_deliverySlot);
                    _deliverySlot.reset();
                    _deliveryCv.notify_one();   // the worker can hand over the next piece
                }
                deliverChunk(chunk);            // without a lock held
            }
        }

        void QueuedVoiceSynthesisProvider::deliverChunk(const Chunk& chunk) {
            const int dstRate = _audioConfig.sampleRate;
            if (chunk.sampleRate <= 0 || dstRate <= 0 || chunk.sampleRate == dstRate) {
                deliverAudio(chunk.pcm.data(), chunk.pcm.size());
                return;
            }
            _resampleBuf.clear();
            _deliveryResampler.reset(chunk.sampleRate, dstRate);
            _deliveryResampler.process(chunk.pcm.data(), chunk.pcm.size(), _resampleBuf);
            _deliveryResampler.flush(_resampleBuf);
            deliverAudio(_resampleBuf.data(), _resampleBuf.size());
        }

        void QueuedVoiceSynthesisProvider::deliverAudio(const float* pcm, size_t n) {
            if (!_resultListener || n == 0) return;

            switch (_audioConfig.format) {
            case AudioFormat::SHORT:
                _int16Buf.resize(n);
                AudioSIMD::convertFloatToInt16(pcm, _int16Buf.data(), n);
                _resultListener->onAudioDataReceived(reinterpret_cast<const char*>(_int16Buf.data()), n * sizeof(int16_t));
                break;
            case AudioFormat::FLOAT:
                _resultListener->onAudioDataReceived(reinterpret_cast<const char*>(pcm), n * sizeof(float));
                break;
            case AudioFormat::INT:
                _int32Buf.resize(n);
                AudioSIMD::convertFloatToInt32(pcm, _int32Buf.data(), n);
                _resultListener->onAudioDataReceived(reinterpret_cast<const char*>(_int32Buf.data()), n * sizeof(int32_t));
                break;
            case AudioFormat::BYTE:
                _uint8Buf.resize(n);
                AudioSIMD::convertFloatToUint8(pcm, _uint8Buf.data(), n);
                _resultListener->onAudioDataReceived(reinterpret_cast<const char*>(_uint8Buf.data()), n * sizeof(uint8_t));
                break;
            default:
                spdlog::warn("[{}] Unknown audio format; delivering as float.", _logTag);
                _resultListener->onAudioDataReceived(reinterpret_cast<const char*>(pcm), n * sizeof(float));
                break;
            }
        }
    } // namespace audio
} // namespace va
