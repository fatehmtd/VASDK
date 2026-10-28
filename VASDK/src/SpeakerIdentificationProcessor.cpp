#include "VASDK/SpeakerIdentificationProcessor.h"
#include "VASDK/AudioSIMD.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <cstring>

namespace va {
    namespace pipeline {

        namespace {
            void normalize(std::vector<float>& v)
            {
                float norm = 0.0f;
                for (float x : v) norm += x * x;
                norm = std::sqrt(norm);
                if (norm > 0.0f) {
                    for (float& x : v) x /= norm;
                }
            }

            float dot(const std::vector<float>& a, const std::vector<float>& b)
            {
                float sum = 0.0f;
                for (size_t i = 0; i < a.size() && i < b.size(); ++i) sum += a[i] * b[i];
                return sum;
            }
        }

        SpeakerIdentificationProcessor::SpeakerIdentificationProcessor(
            std::shared_ptr<audio::SpeakerEmbeddingProvider> provider,
            float threshold,
            float minSpeechSec) : _provider(provider), _threshold(threshold), _minSpeechSec(minSpeechSec)
        {
        }

        void SpeakerIdentificationProcessor::requestEnrollment(const std::string& speakerId)
        {
            std::lock_guard<std::mutex> lock(_speakersMutex);
            _pendingEnrollment = speakerId;
            spdlog::info("[{}] Enrollment requested for '{}' - waiting for the next utterance", getName(), speakerId);
        }

        void SpeakerIdentificationProcessor::addSpeaker(const std::string& speakerId, std::vector<float> embedding)
        {
            normalize(embedding);
            std::lock_guard<std::mutex> lock(_speakersMutex);
            _speakers[speakerId] = std::move(embedding);
        }

        std::map<std::string, std::vector<float>> SpeakerIdentificationProcessor::getSpeakers() const
        {
            std::lock_guard<std::mutex> lock(_speakersMutex);
            return _speakers;
        }

        bool SpeakerIdentificationProcessor::shouldProcess(std::shared_ptr<transport::Frame> frame) const
        {
            if (frame == nullptr) {
                return false;
            }
            switch (frame->getType()) {
            case transport::FrameType::VAD_STATE:
            case transport::FrameType::SPEAKER_ENROLLMENT:
                return true;
            default:
                return false;
            }
        }

        void SpeakerIdentificationProcessor::processFrame(std::shared_ptr<transport::Frame> frame, FlowDirection direction)
        {
            if (frame->getType() == transport::FrameType::SPEAKER_ENROLLMENT) {
                auto enrollFrame = std::static_pointer_cast<transport::SpeakerEnrollmentFrame>(frame);
                if (enrollFrame->status == transport::SpeakerEnrollmentStatus::REQUESTED) {
                    requestEnrollment(enrollFrame->speakerId);
                    return; // consumed - the outcome is emitted after the next utterance
                }
            }
            else if (direction == FlowDirection::DOWNSTREAM) {
                auto vadFrame = std::static_pointer_cast<transport::VADStateFrame>(frame);
                if (vadFrame->state == transport::VADState::ENDED) {
                    processUtterance(*vadFrame);
                }
            }

            if (direction == FlowDirection::DOWNSTREAM) {
                pushDownstream(frame);
            }
            else {
                pushUpstream(frame);
            }
        }

        void SpeakerIdentificationProcessor::processUtterance(const transport::VADStateFrame& vadFrame)
        {
            std::string enrollId;
            {
                std::lock_guard<std::mutex> lock(_speakersMutex);
                enrollId.swap(_pendingEnrollment);
            }

            auto embedding = embedUtterance(vadFrame);
            normalize(embedding);

            if (embedding.empty()) {
                if (!enrollId.empty()) {
                    emitEnrollment(enrollId, transport::SpeakerEnrollmentStatus::FAILED,
                        "utterance too short or could not be embedded");
                }
                return;
            }

            // Track the current speaker: a voice print that no longer matches means the speaker changed.
            const float trackedSimilarity = _trackedSpeaker.empty() ? 0.0f : dot(_trackedSpeaker, embedding);
            const bool isNewSpeaker = _trackedSpeaker.empty() || trackedSimilarity < _threshold;
            if (isNewSpeaker) {
                _trackedSpeaker = embedding;
            }
            else {
                // Sum of unit vectors, renormalized = centroid direction; steadier than one utterance.
                for (size_t i = 0; i < _trackedSpeaker.size(); ++i) _trackedSpeaker[i] += embedding[i];
                normalize(_trackedSpeaker);
            }
            spdlog::info("[{}] {} (similarity to tracked speaker {:.2f}, threshold {:.2f})",
                getName(), isNewSpeaker ? "NEW speaker - tracked voice print updated" : "SAME speaker - voice print refined",
                trackedSimilarity, _threshold);

            if (!enrollId.empty()) {
                addSpeaker(enrollId, std::move(embedding));
                emitEnrollment(enrollId, transport::SpeakerEnrollmentStatus::ENROLLED);
                return;
            }

            std::string bestId;
            float bestScore = -1.0f;
            {
                std::lock_guard<std::mutex> lock(_speakersMutex);
                for (const auto& [id, voicePrint] : _speakers) {
                    if (voicePrint.size() != embedding.size()) continue;
                    const float score = dot(voicePrint, embedding);
                    if (score > bestScore) {
                        bestScore = score;
                        bestId = id;
                    }
                }
            }

            auto idFrame = std::make_shared<transport::SpeakerIdentificationFrame>();
            idFrame->sourceId = getName();
            idFrame->speakerId = bestScore >= _threshold ? bestId : std::string{};
            idFrame->confidence = bestScore;
            idFrame->isNewSpeaker = isNewSpeaker;
            idFrame->trackedSimilarity = trackedSimilarity;
            if (!bestId.empty()) {
                spdlog::info("[{}] Utterance identified as '{}' (best enrolled '{}', score {:.2f})",
                    getName(), idFrame->speakerId, bestId, bestScore);
            }
            pushDownstream(idFrame);
        }

        std::vector<float> SpeakerIdentificationProcessor::embedUtterance(const transport::VADStateFrame& vadFrame)
        {
            if (!_provider || !_ringBuffer) {
                spdlog::warn("[{}] No provider or ring buffer set", getName());
                return {};
            }
            const auto audioConfig = _provider->getAudioConfig();
            const size_t numSamples = vadFrame.totalSpeechLength;
            if (numSamples < static_cast<size_t>(_minSpeechSec * audioConfig.sampleRate)) {
                spdlog::info("[{}] Utterance too short for a speaker check ({:.2f} s < {:.2f} s) - skipped",
                    getName(), static_cast<double>(numSamples) / audioConfig.sampleRate, _minSpeechSec);
                return {};
            }

            _rawBuffer.resize(numSamples * audioConfig.getSampleSizeInBytes());
            int numRead = _ringBuffer->readAbsolute(vadFrame.speechStartOffset, _rawBuffer.data(), 0, static_cast<int>(numSamples));
            if (numRead <= 0) {
                spdlog::warn("[{}] Could not read utterance from ring buffer (offset={}, length={})",
                    getName(), vadFrame.speechStartOffset, numSamples);
                return {};
            }

            _floatBuffer.resize(numRead);
            switch (audioConfig.format) {
            case audio::AudioFormat::BYTE:
                AudioSIMD::convertByteToFloat(reinterpret_cast<const uint8_t*>(_rawBuffer.data()), _floatBuffer.data(), numRead);
                break;
            case audio::AudioFormat::SHORT:
                AudioSIMD::convertInt16ToFloat(reinterpret_cast<const int16_t*>(_rawBuffer.data()), _floatBuffer.data(), numRead);
                break;
            case audio::AudioFormat::INT:
                AudioSIMD::convertInt32ToFloat(reinterpret_cast<const int32_t*>(_rawBuffer.data()), _floatBuffer.data(), numRead);
                break;
            case audio::AudioFormat::FLOAT:
                std::memcpy(_floatBuffer.data(), _rawBuffer.data(), numRead * sizeof(float));
                break;
            default:
                spdlog::warn("[{}] Unsupported audio format: {}", getName(), static_cast<int>(audioConfig.format));
                return {};
            }

            return _provider->computeEmbedding(_floatBuffer.data(), _floatBuffer.size());
        }

        void SpeakerIdentificationProcessor::emitEnrollment(const std::string& speakerId,
            transport::SpeakerEnrollmentStatus status,
            const std::string& errorMessage)
        {
            auto frame = std::make_shared<transport::SpeakerEnrollmentFrame>();
            frame->sourceId = getName();
            frame->speakerId = speakerId;
            frame->status = status;
            frame->errorMessage = errorMessage;
            spdlog::info("[{}] Enrollment of '{}': {} {}", getName(), speakerId, transport::toString(status), errorMessage);
            pushDownstream(frame);
        }

    } // namespace pipeline
} // namespace va
