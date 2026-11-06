#pragma once
#include "GenericVoiceSynthesisProvider.h"
#include "vasdk_export.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace va {
    namespace audio {

        /**
         * @brief Lanczos (a = 3) windowed-sinc resampler that keeps its input history between
         * calls, so audio that arrives in pieces joins without clicks at the edges.
         *
         * reset() before an utterance, process() each piece, flush() at its end. Equal rates
         * (or a rate of 0) pass the audio through.
         */
        class VASDK_EXPORT StreamResampler {
            public:
            void reset(int srcRate, int dstRate);
            /// Appends the samples ready so far to out.
            void process(const float* in, size_t n, std::vector<float>& out);
            /// Appends the rest: the input's last samples, centred in the kernel.
            void flush(std::vector<float>& out);
            bool passthrough() const { return _src == _dst || _src <= 0 || _dst <= 0; }

            private:
            int     _src = 0, _dst = 0, _kernelR = 0;
            int     _lanczosA = 3;          // Lanczos window size (lobes)
            double  _step = 1.0, _filterScale = 1.0;
            std::vector<float> _buf;        // input samples from absolute index _bufStart
            int64_t _bufStart = 0;
            int64_t _nextOut  = 0;

            void produce(std::vector<float>& out, int64_t inputEnd);
            double weight(double x) const;  // Lanczos kernel at x
        };

        /**
         * @brief A voice that runs a model on this machine: everything such a provider needs
         * around the model, so a provider only loads it (loadEngine()) and turns one text into
         * audio (synthesizeOne()).
         *
         * - **A queue.** synthesizeSpeech() queues texts and returns; one worker thread speaks
         *   them in order (acceptsQueuedSynthesis() is true). The listener hears
         *   onSynthesisStarted() before the first text of a sequence and onSynthesisCompleted()
         *   when the queue has drained.
         * - **Cancels.** cancelSynthesis() stops the text being spoken and drops what was queued
         *   before it, never what's queued after it: an interrupting reply sent right after a
         *   cancel is said.
         * - **Audio out.** Either Delivery::Thread: synthesizeOne() hands audio over with
         *   handoffAudio(), and a delivery thread resamples it to the pipeline's rate, converts it
         *   to its format and gives it to the listener while the next piece is synthesized
         *   (finishHandoff() at the end of the text). Or Delivery::Direct: the provider calls
         *   deliverAudio() itself, already at the pipeline's rate (an engine that streams on a
         *   thread of its own).
         *
         * A provider's destructor must call stopVoiceSynthesisProvider(): the worker calls the
         * provider's synthesizeOne(), which is gone once the base class is being destroyed.
         */
        class VASDK_EXPORT QueuedVoiceSynthesisProvider : public GenericVoiceSynthesisProvider {
            public:
            ~QueuedVoiceSynthesisProvider() override;
            QueuedVoiceSynthesisProvider(const QueuedVoiceSynthesisProvider&) = delete;
            QueuedVoiceSynthesisProvider& operator=(const QueuedVoiceSynthesisProvider&) = delete;

            // --- GenericVoiceSynthesisProvider ---
            bool synthesizeSpeech(const std::string& text) final;
            void cancelSynthesis() final;
            bool acceptsQueuedSynthesis() const final { return true; }
            bool startVoiceSynthesisProvider() final;
            void stopVoiceSynthesisProvider() final;
            bool isVoiceSynthesisProviderRunning() const final;
            void setAudioConfiguration(const AudioConfig& config) final;
            AudioConfig getAudioConfiguration() const final;
            void setVoiceSynthesisResultListener(VoiceSynthesisResultListener* listener) final;
            VoiceSynthesisResultListener* getVoiceSynthesisResultListener() const final;

            /// Audio (ms) gathered from each text before any of it is delivered: headroom for a
            /// CPU that's slow to speed up (throttled phones). 0, the default, delivers at once.
            /// Applies to audio given to handoffAudio().
            void setPreBufferedSamplesMS(int ms);
            int  getPreBufferedSamplesMS() const;

            protected:
            enum class Delivery { Thread, Direct };

            /// @param logTag  How the logs name the provider: "[PocketTTS] ..."
            QueuedVoiceSynthesisProvider(std::string logTag, Delivery delivery);

            /// On start, on the caller's thread: load the model (or keep the one loaded).
            /// False fails the start; log why.
            virtual bool loadEngine() = 0;
            /// On the worker thread: speak one queued text. True when it was said in full; false
            /// when cancelled (see cancelled()) or failed (after reportError()). An exception is
            /// reported as an error.
            virtual bool synthesizeOne(const std::string& text) = 0;
            /// After a cancel was requested, on the cancelling thread: stop the engine early.
            virtual void onCancel() {}
            /// On the worker thread, when a sequence was said in full, before onSynthesisCompleted().
            virtual void onSequenceCompleted() {}

            bool cancelled() const { return _cancelSynth.load(); }
            /// The cancel flag itself, for an engine that polls it between steps.
            const std::atomic<bool>& cancelFlag() const { return _cancelSynth; }
            const std::string& logTag() const { return _logTag; }
            VoiceSynthesisResultListener* listener() const { return _resultListener; }
            const AudioConfig& audioConfig() const { return _audioConfig; }
            /// Logs the error and tells the listener.
            void reportError(const std::string& message);

            /// Delivery::Thread, from synthesizeOne(): hand audio at sampleRate to the delivery
            /// thread. Waits while it's still busy with the last piece; drops the audio once
            /// cancelled.
            void handoffAudio(std::vector<float> pcm, int sampleRate);
            /// Delivery::Thread, at the end of synthesizeOne(): hands over what the pre-buffer
            /// holds and waits for the delivery thread to take the last piece. False if cancelled.
            bool finishHandoff();
            /// Converts audio at the pipeline's rate to its format and gives it to the listener.
            /// Delivery::Direct providers call it themselves; one thread at a time.
            void deliverAudio(const float* pcm, size_t n);

            private:
            struct QueuedText {
                std::string text;
                uint64_t    generation;   // the cancel it came after
            };
            struct Chunk {
                std::vector<float> pcm;
                int                sampleRate = 0;
            };

            const std::string _logTag;
            const Delivery    _delivery;
            AudioConfig                   _audioConfig{};
            VoiceSynthesisResultListener* _resultListener = nullptr;
            std::atomic<bool>             _running{false};

            // The queue and the worker. A cancel drops what was queued before it, never what's
            // queued after (an interrupting reply, sent right after the cancel).
            std::thread             _synthThread;
            std::mutex              _queueMutex;
            std::condition_variable _queueCv;
            std::queue<QueuedText>  _textQueue;            // under _queueMutex
            uint64_t                _cancelGeneration{0};  // under _queueMutex
            bool                    _inSynthesisSequence{false};
            std::atomic<bool>       _cancelSynth{false};
            std::atomic<bool>       _stopWorker{false};

            // Delivery::Thread: one slot between the worker and the delivery thread, so the next
            // piece is synthesized while this one is delivered.
            std::thread               _deliveryThread;
            std::mutex                _deliveryMutex;
            std::condition_variable   _deliveryCv;         // the worker waits here: the slot is empty
            std::condition_variable   _deliveryReadyCv;    // delivery waits here: the slot is full
            std::optional<Chunk>      _deliverySlot;
            bool                      _deliveryDone{false};

            // The pre-buffer: the worker's only.
            std::atomic<int>   _preBufferedSamplesMS{0};
            std::vector<float> _preBuffer;
            int                _preBufferRate{0};
            bool               _preBufferFilled{false};

            // The delivery side's buffers: resampling and format conversion.
            StreamResampler      _deliveryResampler;
            std::vector<float>   _resampleBuf;
            std::vector<int16_t> _int16Buf;
            std::vector<int32_t> _int32Buf;
            std::vector<uint8_t> _uint8Buf;

            void workerLoop();
            void deliveryLoop();
            void placeInSlot(std::vector<float> pcm, int sampleRate);
            void flushDeliverySlot();
            void deliverChunk(const Chunk& chunk);
        };
    } // namespace audio
} // namespace va
