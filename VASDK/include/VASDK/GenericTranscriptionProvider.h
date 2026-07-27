#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include "CommonAudioUtils.h"
#include "metrics/MetricsCollector.h"

namespace va
{
    namespace audio
    {
        /**
         * @brief The TranscriptionResult struct
         * Represents a transcription result with text and confidence score.
         */
        struct TranscriptionResult {
            /**
             * @brief LanguageTag
             * A detected language with an associated confidence score.
             * Multiple tags may be present when the model detects code-switching.
             * Tags are ordered by first appearance in the audio.
             */
            struct LanguageTag {
                std::string code;         // ISO 639-1 language code (e.g. "en", "fr")
                float confidence = 0.0f;  // Model confidence for this language (0.0 to 1.0)
            };

            std::string text;
            float confidence = 0.0f;
            int64_t startTimeMs = 0; // Start time of the transcription segment in milliseconds
            int64_t endTimeMs = 0;   // End time of the transcription segment in milliseconds
            std::vector<std::string> alternatives; // Optional alternative transcriptions for the entire result
            std::vector<LanguageTag> languageTags; // Detected languages, ordered by first appearance (e.g. for code-switching scenarios)
            struct Word {
                int64_t startTimeMs = 0; // Start time of the word in milliseconds
                int64_t endTimeMs = 0;   // End time of the word in milliseconds
                std::string text; // The transcribed word text
                float confidence = 0.0f; // Confidence score for the word (0.0 to 1.0)
                std::vector<std::string> alternatives; // Optional alternative transcriptions for the word
                std::vector<LanguageTag> languageTags; // Active language(s) when this word was spoken
            };
            std::vector<Word> words;
        };

        /**
         * @brief The TranscriptionResultListener class
         * Interface for receiving transcription results and events.
         */
        class TranscriptionResultListener
        {
        public:
            TranscriptionResultListener() = default;
            ~TranscriptionResultListener() = default;

            /**
             * @brief onTranscriptionStarted
             * Called when transcription starts.
             */
            virtual void onTranscriptionStarted() = 0;

            /**
             * @brief onTranscriptionStopped
             * Called when transcription stops.
             */
            virtual void onTranscriptionStopped() = 0;

            /**
             * @brief onTranscriptionError
             * Called when an error occurs during transcription.
             * @param error The error message.
             */
            virtual void onTranscriptionError(const std::string &error) = 0;

            /**
             * @brief onPartialTranscription
             * Called when a partial transcription result is available.
             */
            virtual void onPartialTranscription(const TranscriptionResult &result) = 0;

            /**
             * @brief onFinalTranscription
             * Called when a final transcription result is available.
             */
            virtual void onFinalTranscription(const TranscriptionResult &result) = 0;
        };

        /**
         * @brief The GenericTranscriptionProvider class
         */
        class GenericTranscriptionProvider
        {
        public:
            GenericTranscriptionProvider() = default;
            virtual ~GenericTranscriptionProvider() = default;

            /**
             * @brief initializeTranscriptionProvider
             * @return true if the provider initialized successfully, false otherwise
             */
            virtual bool initializeTranscriptionProvider() = 0;

            /**
             * @brief releaseTranscriptionProvider
             * Releases any resources held by the transcription provider
             */
            virtual void releaseTranscriptionProvider() = 0;

            /**
             * @brief setAudioConfiguration
             * Sets the audio configuration for the transcription provider
             */
            virtual void setAudioConfiguration(const AudioConfig &config) = 0;

            /**
             * @brief getAudioConfiguration
             * Gets the audio configuration for the transcription provider
             */
            virtual AudioConfig getAudioConfiguration() const = 0;

            /**
             * @brief writeAudioData
             * Buffers audio for async transcription; results arrive via TranscriptionResultListener.
             */
            virtual size_t writeAudioData(const uint8_t *data, size_t size) = 0;

            /**
             * @brief writeFullAudioData
             * Submits a complete audio segment as a single transcription unit (e.g. pre-recorded
             * files or pre-segmented VAD input), letting the provider optimize for known boundaries.
             * @param data Pointer to the audio data buffer
             * @param size Size of the audio data in bytes
             * @return The transcription result for the segment
             */
            virtual TranscriptionResult writeFullAudioData(const uint8_t *data, size_t size) {
                throw std::runtime_error("writeFullAudioData not implemented by this provider");
            };

            /**
             * @brief setTranscriptionResultListener
             * Sets the listener for transcription results and events
             */
            virtual void setTranscriptionResultListener(TranscriptionResultListener *listener) = 0;

            /**
             * @brief getTranscriptionResultListener
             * @return the current transcription result listener
             */
            virtual TranscriptionResultListener* getTranscriptionResultListener() const = 0;

            /**
             * @brief startTranscriptionProvider
             * @return true if the provider started successfully, false otherwise
             */
            virtual bool startTranscriptionProvider() = 0;

            /**
             * @brief stopTranscriptionProvider
             * Stops the transcription provider
             */
            virtual void stopTranscriptionProvider() = 0;

            /**
             * @brief isTranscriptionProviderRunning
             * @return true if the provider is running, false otherwise
             */
            virtual bool isTranscriptionProviderRunning() const = 0;

            /**
             * @brief commitEndOfSpeech
             * Signals that the current speech segment has ended (e.g. from VAD).
             * The provider should finalize any buffered audio as a final transcription
             * rather than waiting for an idle timeout.
             */
            virtual void commitEndOfSpeech() {}

            /**
             * @brief clearAudioBuffer
             * Discards all in-progress audio accumulated so far for the current segment.
             * Call this on barge-in to discard audio captured while TTS was playing
             * (which may contain speaker echo). Context from the previous completed
             * turn is preserved by the provider at its discretion.
             * Default implementation is a no-op; concrete providers may override.
             */
            virtual void clearAudioBuffer() {}

            /**
             * @brief setTimingObserver
             * Sets a timing observer to receive per-stage inference latency events (e.g. "encoder", "decoder").
             * Default implementation is a no-op; concrete providers may override.
             */
            virtual void setTimingObserver(va::metrics::TimingObserverInterface* /*observer*/) {}
        };
    } // namespace audio
} // namespace va
