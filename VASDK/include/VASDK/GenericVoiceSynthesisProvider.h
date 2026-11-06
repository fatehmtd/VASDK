#pragma once
#include "TypeName.h"
#include "vasdk_export.h"
#include <string>
#include "CommonAudioUtils.h"

namespace va {
    namespace audio {

        /**
         * @brief The VoiceSynthesisResultListener class
         * Listener interface for receiving voice synthesis results and events
         */
        class VoiceSynthesisResultListener {
            public:
            VoiceSynthesisResultListener() = default;
            virtual ~VoiceSynthesisResultListener() = default;

            /**
             * @brief Called when synthesis starts
             */
            virtual void onSynthesisStarted() = 0;

            /**
             * @brief Called when synthesis is completed
             */
            virtual void onSynthesisCompleted() = 0;

            /**
             * @brief Called when an error occurs during synthesis
             * @param error The error message
             */
            virtual void onSynthesisError(const std::string& error) = 0;

            /**
             * @brief Called when audio data is received
             * @param data The audio data
             * @param size The size of the audio data
             */
            virtual void onAudioDataReceived(const char* data, size_t size) = 0;
        };

        /**
         * @brief How a text is said: another of the provider's own voices, or another pace.
         * The default, an empty voice and speed 0, is the provider's usual way.
         */
        struct SpeechStyle {
            std::string voice;   // a voice the provider has ("cosette", "Ava"); empty: its own
            float speed = 0;     // 1.2 faster, 0.8 slower; 0: as usual
            bool isDefault() const { return voice.empty() && speed <= 0; }
            bool operator==(const SpeechStyle& other) const { return voice == other.voice && speed == other.speed; }
            bool operator!=(const SpeechStyle& other) const { return !(*this == other); }
        };

        /**
         * @brief The GenericVoiceSynthesisEngine class
         */
        class GenericVoiceSynthesisProvider {
            public:
            GenericVoiceSynthesisProvider() = default;
            virtual ~GenericVoiceSynthesisProvider() = default;
            virtual std::string getName() const { return shortTypeName(typeid(*this)); }  ///< for logs and UIs

            /**
             * @brief synthesizes speech from text
             * @param text The text to be synthesized
             */
            virtual bool synthesizeSpeech(const std::string& text) = 0;

            /**
             * @brief Says `text` in `style`: a part of a reply in another voice, or slower (an
             * agent's `say { voice speed }`). A provider that can't change them says it as
             * synthesizeSpeech() does, the default.
             */
            virtual bool synthesizeStyledSpeech(const std::string& text, const SpeechStyle& style) {
                (void)style;
                return synthesizeSpeech(text);
            }

            /**
             * @brief Cancel any in-progress synthesis immediately.
             *
             * Must be non-blocking. The provider should stop emitting audio and
             * call the listener's onSynthesisCompleted() (or onSynthesisError())
             * once the cancellation is confirmed, so upstream processors receive
             * a SynthesisStateFrame{INTERRUPTED} via TTSFrameProcessor.
             *
             * Default no-op - providers that support cancellation should override.
             */
            virtual void cancelSynthesis() {}

            /**
             * @brief Whether synthesizeSpeech() may be called again before the previous call
             * finished: the provider queues calls and plays them back in order.
             *
             * TTSFrameProcessor speaks a reply sentence by sentence. With false (the safe
             * default) it sends the next sentence only after onSynthesisCompleted() for the
             * previous one; with true it hands sentences over as soon as they arrive.
             */
            virtual bool acceptsQueuedSynthesis() const { return false; }

            /**
             * @brief Whether synthesizeSpeech() accepts SSML (a <speak> document). With false (the
             * default) TTSFrameProcessor sends plain text: the frame's text, or its SSML untagged.
             */
            virtual bool supportsSsml() const { return false; }
            /**
             * Whether the voice reads dates, times and phone numbers well by itself. When not
             * (the default), TTSFrameProcessor gives it nlu::english::speakable() text: "2026-11-10"
             * as "Tuesday, November 10th", "555-123-4567" digit by digit.
             */
            virtual bool normalizesText() const { return false; }

            /**
             * @brief startVoiceSynthesisProvider
             * @return true if the provider started successfully, false otherwise
             */
            virtual bool startVoiceSynthesisProvider() = 0;

            /**
             * @brief stopVoiceSynthesisProvider
             * Stops the voice synthesis provider
             */
            virtual void stopVoiceSynthesisProvider() = 0;

            /**
             * @brief isVoiceSynthesisProviderRunning
             * @return true if the provider is running, false otherwise
             */
            virtual bool isVoiceSynthesisProviderRunning() const = 0;

            /**
             * @brief setAudioConfiguration
             * Sets the audio configuration for the voice synthesis provider
             */
            virtual void setAudioConfiguration(const AudioConfig& config) = 0;

            /**
             * @brief getAudioConfiguration
             * Gets the audio configuration for the voice synthesis provider
             */
            virtual AudioConfig getAudioConfiguration() const = 0;

            /**
             * @brief setVoiceSynthesisResultListener
             * Sets the listener for receiving voice synthesis results and events
             */
            virtual void setVoiceSynthesisResultListener(VoiceSynthesisResultListener* listener) = 0;

            /**
             * @brief getVoiceSynthesisResultListener
             * Gets the listener for receiving voice synthesis results and events
             */
            virtual VoiceSynthesisResultListener* getVoiceSynthesisResultListener() const = 0;
        };
    } // namespace audio
} // namespace va
