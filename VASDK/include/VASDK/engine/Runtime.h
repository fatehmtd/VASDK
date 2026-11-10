#pragma once

#include <VASDK/vasdk_export.h>
#include "../CommonAudioUtils.h"
#include "ProviderRegistry.h"
#include "Session.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace va {

class AudioRingBuffer;
namespace audio {
class VADProvider;
class GenericTranscriptionProvider;
class GenericVoiceSynthesisProvider;
class TurnDetectionProvider;
class WakeWordProvider;
class GenericAudioDSPProvider;
class SpeakerEmbeddingProvider;
}
namespace nlu { class SentenceEncoder; class EntityExtractor; }
namespace llm { class Completion; class LLMTransport; }
namespace transport { class AudioTransport; }

namespace engine {

/**
 * @brief Applies the config's `audio` block over the app's formats: each value set there replaces
 *        the one passed in. Runtime::create does this itself; call it too when the app opens
 *        audio devices before creating the runtime, so they open in the same format.
 * @return false, with `error` set, for a value no pipeline can use (a zero sample rate, say).
 */
VASDK_EXPORT bool applyAudioConfig(const vasdk::engine::Audio& config, audio::AudioConfig& inputAudio,
                                   audio::AudioConfig& outputAudio, int& frameSize, std::string* error = nullptr);

/**
 * @brief What many conversations share: the engine config and the providers it names.
 *
 * Create one per process, then one Session per conversation. A session takes a set of
 * providers (VAD, STT, TTS, turn model) for as long as it runs; when it ends, the set goes back
 * here and the next session reuses it. Models therefore load once per conversation running at
 * the same time, not once per conversation.
 *
 * @code
 * auto runtime = va::engine::Runtime::create(config, registry, {}, &error);
 * va::engine::Session::Options call;
 * call.id = "call-42";
 * call.transport = callTransport;
 * auto session = runtime->createSession(call, &error);
 * session->start();
 * @endcode
 */
class VASDK_EXPORT Runtime : public std::enable_shared_from_this<Runtime> {
public:
    struct Options {
        /** Used when the config's models_dir is empty; both empty: ModelStore::defaultDirectory(). */
        std::string modelsDir;
        audio::AudioConfig inputAudio{16000, audio::AudioFormat::SHORT, 1};
        audio::AudioConfig outputAudio{16000, audio::AudioFormat::SHORT, 1};
        int frameSize = 1600;     ///< samples per frame: 100 ms at 16 kHz, the VAD/turn decision grid
        size_t maxSessions = 0;   ///< sessions running at once; 0 = no limit
    };

    /** One session's providers, with the mic ring buffer the turn model reads. */
    struct ProviderSet {
        std::shared_ptr<AudioRingBuffer> ringBuffer;
        std::shared_ptr<audio::VADProvider> vad;
        std::shared_ptr<audio::GenericTranscriptionProvider> stt;
        std::shared_ptr<audio::GenericVoiceSynthesisProvider> tts;
        std::shared_ptr<audio::TurnDetectionProvider> turn;   ///< null: the VAD's silence ends the turn
        std::shared_ptr<audio::WakeWordProvider> wakeWord;    ///< only in the WAKE_WORD and HYBRID modes
        std::shared_ptr<audio::GenericAudioDSPProvider> audioDsp;           ///< null without providers.audio_dsp
        std::shared_ptr<audio::SpeakerEmbeddingProvider> speakerEmbedding;  ///< null without providers.speaker_embedding
        /// What every stage hears: audioDsp's output, written in step with ringBuffer (which the
        /// transport fills); ringBuffer itself without an audioDsp.
        std::shared_ptr<AudioRingBuffer> processedRingBuffer;
        std::string sttId, ttsId;                             ///< the ids that started
        /// The STT and TTS `fallback` providers after the ones that started, for when the network
        /// drops (loadFallbacks); null until loaded, or without one. Kept with the set once loaded.
        std::shared_ptr<audio::GenericTranscriptionProvider> sttFallback;
        std::shared_ptr<audio::GenericVoiceSynthesisProvider> ttsFallback;
        std::string sttFallbackId, ttsFallbackId;
    };

    /**
     * Creates the first set of providers right away, so a misconfigured provider fails here.
     * nullptr, with `error` set ("STT: parakeet: not built into this binary"), on failure.
     */
    static std::shared_ptr<Runtime> create(const vasdk::engine::EngineConfig& config, const ProviderRegistry& registry,
                                           Options options, std::string* error = nullptr);
    /** Loads the config file (text proto, JSON or binary; ${ENV} expanded), then create(). */
    static std::shared_ptr<Runtime> create(const std::string& configPath, const ProviderRegistry& registry,
                                           Options options, std::string* error = nullptr);

    /** A new conversation; nullptr, with `error` set, at maxSessions or if a provider fails. */
    std::unique_ptr<Session> createSession(Session::Options options, std::string* error = nullptr);

    const vasdk::engine::EngineConfig& config() const { return _config; }
    /** The config's sentence_encoder, shared by every session; null without one. */
    std::shared_ptr<const nlu::SentenceEncoder> sentenceEncoder() const { return _sentenceEncoder; }
    /** The config's entity_model when it's a provider (gliner), shared by every session; null for none,
     *  and for tagger and llm, which a session makes from its agent and its LLM. */
    std::shared_ptr<const nlu::EntityExtractor> entityExtractor() const { return _entityExtractor; }
    const Options& options() const { return _options; }
    size_t activeSessions() const;
    /** Provider sets loaded and waiting for a session. */
    size_t idleProviderSets() const;

    /**
     * The transport the config names, when it names one: a webrtc peer (`sendAllFrames` true: the
     * page shows the session's state), else the audio_input and audio_output devices. nullptr with
     * `error` empty when it names none (the app brings the audio); nullptr with `error` set when
     * one fails to start.
     */
    std::shared_ptr<transport::AudioTransport> configTransport(bool* sendAllFrames, std::string* error) const;
    /** One session's LLM from the config's providers.llm. */
    struct LlmProvider {
        std::shared_ptr<llm::LLMTransport> transport;      ///< the conversation's
        std::shared_ptr<const llm::Completion> completion; ///< the agent's; null when the provider has none
    };
    /** Empty, with `error` empty, when the config names none; empty with `error` set when it fails. */
    LlmProvider createLlm(std::string* error) const;

    /**
     * Loads the set's STT and TTS fallbacks, if not yet: the config's `fallback` providers after
     * the one that started (local ones, for when the network drops). Takes as long as loading
     * models does. False, with `error` set, when neither has one that starts.
     */
    bool loadFallbacks(ProviderSet& providers, std::string* error = nullptr) const;
    /** For Session: a set to use until release(). */
    std::shared_ptr<ProviderSet> acquire(std::string* error);
    void release(std::shared_ptr<ProviderSet> providers);

private:
    Runtime(vasdk::engine::EngineConfig config, ProviderRegistry registry, Options options);
    std::shared_ptr<ProviderSet> createProviderSet(std::string* error) const;

    vasdk::engine::EngineConfig _config;
    ProviderRegistry _registry;
    Options _options;
    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<ProviderSet>> _idle;
    size_t _active = 0;
    std::shared_ptr<const nlu::SentenceEncoder> _sentenceEncoder;
    std::shared_ptr<const nlu::EntityExtractor> _entityExtractor;
};

} // namespace engine
} // namespace va
