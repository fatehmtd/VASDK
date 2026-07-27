#pragma once
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
         * @brief The GenericVoiceSynthesisEngine class
         */
        class GenericVoiceSynthesisProvider {
            public:
            GenericVoiceSynthesisProvider() = default;
            virtual ~GenericVoiceSynthesisProvider() = default;

            /**
             * @brief synthesizes speech from text
             * @param text The text to be synthesized
             */
            virtual bool synthesizeSpeech(const std::string& text) = 0;

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
