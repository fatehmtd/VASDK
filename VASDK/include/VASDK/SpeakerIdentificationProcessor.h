#pragma once

#include "FrameProcessor.h"
#include "SpeakerEmbeddingProvider.h"
#include "AudioRingBuffer.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace va {
    namespace pipeline {

        /**
         * @brief Speaker identification processor - enrolls users by voice and tags each
         *        utterance with the enrolled speaker who said it.
         *
         * On every VAD ENDED the utterance is read from the ring buffer and embedded by the
         * SpeakerEmbeddingProvider. The embedding is compared to the tracked (current) speaker:
         * below the threshold it is a new speaker and replaces the tracked voice print, otherwise
         * it is averaged into it. Then:
         *   - if an enrollment is pending, the embedding is stored under that speaker id and a
         *     SpeakerEnrollmentFrame (ENROLLED / FAILED) is emitted downstream;
         *   - otherwise a SpeakerIdentificationFrame is emitted downstream with the tracking
         *     result and the best enrolled match (empty speakerId when no enrolled speaker
         *     scores above the threshold).
         *
         * Enrollment is requested with requestEnrollment() or by a SpeakerEnrollmentFrame with
         * status REQUESTED. Place it after VADFrameProcessor and before transcription so the
         * identification precedes the utterance's TRANSCRIPTION frame.
         */
        class VASDK_EXPORT SpeakerIdentificationProcessor : public FrameProcessor {
            public:
            /**
             * @param provider       Embedding model.
             * @param threshold      Minimum cosine similarity to accept a match. The default matches
             *                       pyannote 3.1's clustering threshold (cosine distance 0.7045).
             * @param minSpeechSec   Utterances shorter than this are not embedded.
             */
            SpeakerIdentificationProcessor(std::shared_ptr<audio::SpeakerEmbeddingProvider> provider,
                float threshold = 0.3f,
                float minSpeechSec = 1.0f);

            void setRingBuffer(std::shared_ptr<AudioRingBuffer> ringBuffer) { _ringBuffer = ringBuffer; }

            /** Enroll the next utterance as speakerId (replaces an existing voice print). */
            void requestEnrollment(const std::string& speakerId);

            /** Restore a voice print saved from getSpeakers(). */
            void addSpeaker(const std::string& speakerId, std::vector<float> embedding);

            /** Enrolled voice prints (L2-normalized), keyed by speaker id. */
            std::map<std::string, std::vector<float>> getSpeakers() const;

            //  FrameProcessor interface

            std::string getName() const override { return "SpeakerIdentificationProcessor"; }
            bool shouldProcess(std::shared_ptr<transport::Frame> frame) const override;
            void processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction) override;

            private:
            void processUtterance(const transport::VADStateFrame& vadFrame);
            std::vector<float> embedUtterance(const transport::VADStateFrame& vadFrame);
            void emitEnrollment(const std::string& speakerId, transport::SpeakerEnrollmentStatus status,
                const std::string& errorMessage = {});

            std::shared_ptr<audio::SpeakerEmbeddingProvider> _provider;
            std::shared_ptr<AudioRingBuffer> _ringBuffer;
            float _threshold;
            float _minSpeechSec;
            std::vector<char>  _rawBuffer;
            std::vector<float> _floatBuffer;

            mutable std::mutex _speakersMutex;  // guards _speakers and _pendingEnrollment (app thread vs processing thread)
            std::map<std::string, std::vector<float>> _speakers;
            std::string _pendingEnrollment;
            std::vector<float> _trackedSpeaker;  // current speaker's voice print, processing thread only
        };

    } // namespace pipeline
} // namespace va
