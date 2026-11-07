// The C API's providers written in the app (VASDK/c/vasdk.h, vasdk_providers_add_*): one adapter
// per provider interface, registered by id, so an engine config names them like built-in ones.

#include "c/vasdk.h"
#include "c/vasdk_cpp.h"
#include "vasdk_c_internal.h"

#include "AudioRingBuffer.h"
#include "Frame.h"
#include "GenericTranscriptionProvider.h"
#include "GenericVoiceSynthesisProvider.h"
#include "TurnDetectionProvider.h"
#include "VADProvider.h"
#include "WakeWordProvider.h"
#include "engine/ProviderRegistry.h"
#include "llm/Completion.h"
#include "llm/LLMTransport.h"
#include "nlu/EmbeddingNluProvider.h"
#include "nlu/EntityExtractor.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// What a provider gives back through: the listener the pipeline set on it.
struct vasdk_sink {
    std::mutex mutex;
    va::audio::TranscriptionResultListener* stt = nullptr;
    va::audio::VoiceSynthesisResultListener* tts = nullptr;
    bool speaking = false;   // a TTS has started the current text
    // Batch transcription (writeFullAudioData): the final given meanwhile is its result
    bool capturing = false;
    bool captured = false;
    std::string final;
};

namespace {

using va::engine::ProviderContext;

struct Callbacks {
    vasdk_provider_create_fn create = nullptr;
    vasdk_provider_destroy_fn destroy = nullptr;
    void* user = nullptr;
};

// One provider instance of the app's: made at construction, ended at destruction, with its sink.
class Instance {
public:
    Instance(const Callbacks& callbacks, int sampleRate) : _destroy(callbacks.destroy), _sink(std::make_unique<vasdk_sink>()) {
        _instance = callbacks.create(_sink.get(), sampleRate, callbacks.user);
    }
    ~Instance() {
        if (_destroy && _instance) _destroy(_instance);
    }
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    bool started() const { return _instance != nullptr; }
    void* get() const { return _instance; }
    vasdk_sink* sink() const { return _sink.get(); }

private:
    vasdk_provider_destroy_fn _destroy;
    std::unique_ptr<vasdk_sink> _sink;
    void* _instance = nullptr;
};

bool sixteenBitMono(const va::audio::AudioConfig& config, const std::string& id) {
    if (config.format == va::audio::AudioFormat::SHORT && config.channels == 1) return true;
    spdlog::error("[vasdk] provider {}: the C API's providers take 16-bit mono PCM", id);
    return false;
}

const short* samplesOf(const char* bytes) { return reinterpret_cast<const short*>(bytes); }

//  VAD

class AppVad : public va::audio::VADProvider {
public:
    AppVad(std::string id, const Callbacks& callbacks, int sampleRate, vasdk_vad_fn detect)
        : _id(std::move(id)), _instance(callbacks, sampleRate), _detect(detect) {}
    std::string getName() const override { return _id; }
    bool started() const { return _instance.started(); }
    bool initialize(const va::audio::AudioConfig& audioConfig, int) override {
        _audioConfig = audioConfig;
        return sixteenBitMono(audioConfig, _id);
    }
    void release() override {}
    std::vector<VADResult> detectVoiceActivity(const char* buffer, size_t offset, size_t numSamples) const override {
        if (!buffer || numSamples == 0) return {};
        const float confidence = _detect(_instance.get(), samplesOf(buffer) + offset, numSamples);
        if (confidence < 0.5f) return {};
        return {{offset, numSamples, std::clamp(confidence, 0.0f, 1.0f)}};
    }

private:
    std::string _id;
    Instance _instance;
    vasdk_vad_fn _detect;
};

//  STT

class AppStt : public va::audio::GenericTranscriptionProvider {
public:
    AppStt(std::string id, const Callbacks& callbacks, int sampleRate, vasdk_stt_event_fn start, vasdk_stt_write_fn write,
           vasdk_stt_event_fn endOfSpeech, vasdk_stt_event_fn stop)
        : _id(std::move(id)), _instance(callbacks, sampleRate), _start(start), _endOfSpeech(endOfSpeech), _stop(stop), _write(write) {}
    std::string getName() const override { return _id; }
    bool started() const { return _instance.started(); }
    bool initializeTranscriptionProvider() override { return sixteenBitMono(_config, _id); }
    void releaseTranscriptionProvider() override {}
    void setAudioConfiguration(const va::audio::AudioConfig& config) override { _config = config; }
    va::audio::AudioConfig getAudioConfiguration() const override { return _config; }
    size_t writeAudioData(const uint8_t* data, size_t size) override {
        if (data && size >= sizeof(short)) _write(_instance.get(), reinterpret_cast<const short*>(data), size / sizeof(short));
        return size;
    }
    // Streaming transcription off: the whole utterance, then end_of_speech; the final the app
    // gives before end_of_speech returns is the result.
    va::audio::TranscriptionResult writeFullAudioData(const uint8_t* data, size_t size) override {
        auto* sink = _instance.sink();
        {
            std::lock_guard<std::mutex> lock(sink->mutex);
            sink->capturing = true;
            sink->captured = false;
            sink->final.clear();
        }
        writeAudioData(data, size);
        if (_endOfSpeech) _endOfSpeech(_instance.get());
        va::audio::TranscriptionResult result;
        std::lock_guard<std::mutex> lock(sink->mutex);
        sink->capturing = false;
        if (sink->captured) {
            result.text = sink->final;
            result.confidence = 1.0f;
        }
        return result;
    }
    void setTranscriptionResultListener(va::audio::TranscriptionResultListener* listener) override {
        std::lock_guard<std::mutex> lock(_instance.sink()->mutex);
        _instance.sink()->stt = listener;
    }
    va::audio::TranscriptionResultListener* getTranscriptionResultListener() const override {
        std::lock_guard<std::mutex> lock(_instance.sink()->mutex);
        return _instance.sink()->stt;
    }
    bool startTranscriptionProvider() override {
        if (_start) _start(_instance.get());
        _running = true;
        return true;
    }
    void stopTranscriptionProvider() override {
        if (_running && _stop) _stop(_instance.get());
        _running = false;
    }
    bool isTranscriptionProviderRunning() const override { return _running; }
    void commitEndOfSpeech() override {
        if (_endOfSpeech) _endOfSpeech(_instance.get());
    }

private:
    std::string _id;
    Instance _instance;
    vasdk_stt_event_fn _start, _endOfSpeech, _stop;
    vasdk_stt_write_fn _write;
    va::audio::AudioConfig _config{};
    std::atomic<bool> _running{false};
};

//  TTS

class AppTts : public va::audio::GenericVoiceSynthesisProvider {
public:
    AppTts(std::string id, const Callbacks& callbacks, int sampleRate, vasdk_tts_synthesize_fn synthesize, vasdk_tts_cancel_fn cancel)
        : _id(std::move(id)), _instance(callbacks, sampleRate), _synthesize(synthesize), _cancel(cancel) {}
    std::string getName() const override { return _id; }
    bool started() const { return _instance.started(); }
    bool synthesizeSpeech(const std::string& text) override { return _synthesize(_instance.get(), text.c_str()) != 0; }
    void cancelSynthesis() override {
        if (_cancel) _cancel(_instance.get());
        std::lock_guard<std::mutex> lock(_instance.sink()->mutex);
        _instance.sink()->speaking = false;
    }
    bool startVoiceSynthesisProvider() override {
        _running = true;
        return sixteenBitMono(_config, _id);
    }
    void stopVoiceSynthesisProvider() override { _running = false; }
    bool isVoiceSynthesisProviderRunning() const override { return _running; }
    void setAudioConfiguration(const va::audio::AudioConfig& config) override { _config = config; }
    va::audio::AudioConfig getAudioConfiguration() const override { return _config; }
    void setVoiceSynthesisResultListener(va::audio::VoiceSynthesisResultListener* listener) override {
        std::lock_guard<std::mutex> lock(_instance.sink()->mutex);
        _instance.sink()->tts = listener;
    }
    va::audio::VoiceSynthesisResultListener* getVoiceSynthesisResultListener() const override {
        std::lock_guard<std::mutex> lock(_instance.sink()->mutex);
        return _instance.sink()->tts;
    }

private:
    std::string _id;
    Instance _instance;
    vasdk_tts_synthesize_fn _synthesize;
    vasdk_tts_cancel_fn _cancel;
    va::audio::AudioConfig _config{};
    std::atomic<bool> _running{false};
};

//  End of turn: SmartTurnV3's decisions (VASDK_3P_IMPL/SmartTurnV3), on the app's probability

class AppTurn : public va::audio::TurnDetectionProvider {
public:
    AppTurn(std::string id, const Callbacks& callbacks, const ProviderContext& context, vasdk_turn_fn evaluate)
        : _id(std::move(id)), _instance(callbacks, static_cast<int>(context.audio.sampleRate)), _evaluate(evaluate),
          _ringBuffer(context.ringBuffer), _maxSamples(static_cast<size_t>(context.audio.sampleRate) * kMaxSeconds) {}
    std::string getName() const override { return _id; }
    bool started() const { return _instance.started() && _ringBuffer; }

    TurnEvaluation evaluate(const std::shared_ptr<va::transport::Frame> frame, bool agentIsSpeaking) override {
        using va::transport::TurnStateFrame;
        using va::transport::VADState;
        if (frame->getType() == va::transport::FrameType::VAD_STATE) {
            const auto vad = std::static_pointer_cast<va::transport::VADStateFrame>(frame);
            _paused = vad->state == VADState::PAUSED || vad->state == VADState::ENDING;
            if (vad->state == VADState::STARTING) {
                _speaking = true;
                _heard = true;
                _speechStart = static_cast<long>(vad->speechStartOffset);
                _framesAfter = 0;
                return {Decision::TURN_START, vad->confidence, TurnStateFrame::TurnActor::USER,
                        agentIsSpeaking ? TurnStateFrame::TurnType::INTERRUPT : TurnStateFrame::TurnType::NORMAL};
            }
            if (vad->state == VADState::ENDED) _speaking = false;
            return {};
        }
        if (frame->getType() != va::transport::FrameType::AUDIO || !_heard) return {};
        const bool inPause = _speaking && evaluateOnPause() && _paused;
        if ((_speaking && !inPause) || (!inPause && _framesAfter >= kFramesAfter)) return {};
        const auto audio = std::static_pointer_cast<va::transport::AudioFrame>(frame);
        const long available = _ringBuffer->computeDeltaOffset(_speechStart, static_cast<long>(audio->ringbufferOffset));
        const long count = std::min(available, static_cast<long>(_maxSamples));
        if (count <= 0) return {};
        _samples.resize(static_cast<size_t>(count));
        if (_ringBuffer->readAbsolute(_speechStart + (available - count), reinterpret_cast<char*>(_samples.data()), 0,
                                      static_cast<int>(count)) < 0)
            return {};
        const float probability = _evaluate(_instance.get(), _samples.data(), _samples.size());
        if (!inPause) ++_framesAfter;
        if (probability < (inPause ? pauseThreshold() : kThreshold)) return {};
        _framesAfter = 0;
        _heard = false;
        _paused = false;
        return {Decision::TURN_END, probability, TurnStateFrame::TurnActor::USER, TurnStateFrame::TurnType::NORMAL};
    }

private:
    static constexpr size_t kMaxSeconds = 8;   // the most audio a turn model is given
    static constexpr int kFramesAfter = 10;    // frames asked after the speech ended
    static constexpr float kThreshold = 0.5f;

    std::string _id;
    Instance _instance;
    vasdk_turn_fn _evaluate;
    std::shared_ptr<va::AudioRingBuffer> _ringBuffer;
    size_t _maxSamples;
    std::vector<short> _samples;
    bool _speaking = false, _heard = false, _paused = false;
    long _speechStart = 0;
    int _framesAfter = 0;
};

//  Wake word

class AppWakeWord : public va::audio::WakeWordProvider {
public:
    AppWakeWord(std::string id, const Callbacks& callbacks, int sampleRate, vasdk_wake_word_fn process)
        : _id(std::move(id)), _instance(callbacks, sampleRate), _process(process) {}
    std::string getName() const override { return _id; }
    bool started() const { return _instance.started(); }
    bool initialize(const va::audio::AudioConfig& audioConfig) override { return sixteenBitMono(audioConfig, _id); }
    std::optional<Detection> processAudio(const char* samples, size_t numSamples) override {
        if (!samples || numSamples == 0) return std::nullopt;
        vasdk_result phrase;
        const float confidence = _process(_instance.get(), samplesOf(samples), numSamples, &phrase);
        if (confidence <= 0.0f || !phrase.set || phrase.failed) return std::nullopt;
        return Detection{phrase.json, std::clamp(confidence, 0.0f, 1.0f)};
    }

private:
    std::string _id;
    Instance _instance;
    vasdk_wake_word_fn _process;
};

//  Sentence encoder and entity model: one instance, shared by every session of a runtime

class AppEncoder : public va::nlu::SentenceEncoder {
public:
    AppEncoder(std::string id, const Callbacks& callbacks, vasdk_encoder_fn encode)
        : _id(std::move(id)), _instance(callbacks, 0), _encode(encode) {}
    bool started() const { return _instance.started(); }
    std::string name() const override { return _id; }
    std::vector<float> encode(const std::string& sentence) const override {
        vasdk_result vector;
        _encode(_instance.get(), sentence.c_str(), &vector);
        return vector.hasFloats && !vector.failed ? vector.floats : std::vector<float>();
    }

private:
    std::string _id;
    Instance _instance;
    vasdk_encoder_fn _encode;
};

class AppEntityModel : public va::nlu::EntityExtractor {
public:
    AppEntityModel(std::string id, const Callbacks& callbacks, vasdk_entity_model_fn extract)
        : _id(std::move(id)), _instance(callbacks, 0), _extract(extract) {}
    bool started() const { return _instance.started(); }
    std::string name() const override { return _id; }
    std::vector<va::nlu::EntitySpan> extract(const std::vector<std::string>& words, const std::vector<va::nlu::EntityLabel>& labels,
                                             bool sensitive) const override {
        nlohmann::json request{{"words", words}, {"sensitive", sensitive}, {"labels", nlohmann::json::array()}};
        for (const auto& label : labels)
            request["labels"].push_back({{"entity", label.entity}, {"label", label.label}, {"description", label.description},
                                         {"examples", label.examples}});
        vasdk_result result;
        _extract(_instance.get(), request.dump().c_str(), &result);
        std::vector<va::nlu::EntitySpan> spans;
        if (!result.set || result.failed) return spans;
        const auto found = nlohmann::json::parse(result.json, nullptr, false);
        if (!found.is_array()) {
            spdlog::warn("[vasdk] entity model {} didn't return an array: {}", _id, result.json);
            return spans;
        }
        for (const auto& entry : found) {
            if (!entry.is_object() || !entry.contains("entity")) continue;
            va::nlu::EntitySpan span;
            span.entity = entry.value("entity", std::string());
            span.start = entry.value("start", static_cast<size_t>(0));
            span.length = entry.value("length", static_cast<size_t>(1));
            span.score = entry.value("score", 1.0f);
            if (span.length > 0 && span.start + span.length <= words.size()) spans.push_back(std::move(span));
        }
        return va::nlu::nonOverlapping(std::move(spans));
    }

private:
    std::string _id;
    Instance _instance;
    vasdk_entity_model_fn _extract;
};

//  LLM: the conversation's transport and the agent's one-shot asks, through the same callback

std::string requestJson(const std::vector<va::llm::ConversationMessage>& messages) {
    nlohmann::json request{{"messages", nlohmann::json::array()}};
    for (const auto& message : messages) {
        const char* role = message.role == va::llm::MessageRole::SYSTEM      ? "system"
                           : message.role == va::llm::MessageRole::ASSISTANT ? "assistant"
                           : message.role == va::llm::MessageRole::TOOL      ? "tool"
                                                                             : "user";
        request["messages"].push_back({{"role", role}, {"content", message.content}});
    }
    return request.dump();
}

std::optional<std::string> ask(vasdk_llm_fn complete, void* instance, const std::vector<va::llm::ConversationMessage>& messages,
                               const std::string& id) {
    vasdk_result reply;
    complete(instance, requestJson(messages).c_str(), &reply);
    if (reply.failed) {
        spdlog::warn("[vasdk] LLM {}: {}", id, reply.error);
        return std::nullopt;
    }
    if (!reply.set) return std::nullopt;
    return reply.json;
}

class AppLlm : public va::llm::LLMTransport {
public:
    AppLlm(std::string id, const Callbacks& callbacks, int sampleRate, vasdk_llm_fn complete, vasdk_llm_cancel_fn cancel)
        : LLMTransport(va::llm::LLMProvider::CUSTOM, va::llm::LLMConfig{}), _id(std::move(id)), _instance(callbacks, sampleRate),
          _complete(complete), _cancel(cancel) {}
    std::string getName() const override { return _id; }
    bool started() const { return _instance.started(); }
    bool initialize() override {
        _initialized = true;
        return true;
    }
    void cancel() override {
        if (_cancel) _cancel(_instance.get());
    }

protected:
    std::string sendLLMRequest(const std::string&) override {
        std::vector<va::llm::ConversationMessage> messages;
        std::string system = _config.systemPrompt;
        for (const auto& [name, resolve] : _templateVarResolvers) {
            const std::string placeholder = "{" + name + "}";
            for (auto at = system.find(placeholder); at != std::string::npos; at = system.find(placeholder, at)) {
                const std::string value = resolve(getPromptChatFormat());
                system.replace(at, placeholder.size(), value);
                at += value.size();
            }
        }
        if (!_responseFormatInstruction.empty() && system.find(_responseFormatInstruction) == std::string::npos)
            system += (system.empty() ? "" : "\n\n") + _responseFormatInstruction;
        if (!system.empty()) messages.push_back({va::llm::MessageRole::SYSTEM, system, 0});
        {
            std::lock_guard<std::mutex> lock(_historyMutex);
            for (const auto& message : _conversationHistory)
                if (message.role != va::llm::MessageRole::SYSTEM) messages.push_back(message);
        }
        if (auto reply = ask(_complete, _instance.get(), messages, _id)) return *reply;
        // No reply: the user's message added for it goes, or the next request has two in a row
        std::lock_guard<std::mutex> lock(_historyMutex);
        if (!_conversationHistory.empty() && _conversationHistory.back().role == va::llm::MessageRole::USER)
            _conversationHistory.pop_back();
        return "";
    }

private:
    std::string _id;
    Instance _instance;
    vasdk_llm_fn _complete;
    vasdk_llm_cancel_fn _cancel;
};

class AppCompletion : public va::llm::Completion {
public:
    AppCompletion(std::string id, const Callbacks& callbacks, vasdk_llm_fn complete)
        : _id(std::move(id)), _instance(callbacks, 0), _complete(complete) {}
    bool started() const { return _instance.started(); }
    std::optional<std::string> complete(const std::vector<va::llm::ConversationMessage>& messages) const override {
        return ask(_complete, _instance.get(), messages, _id);
    }

private:
    std::string _id;
    Instance _instance;
    vasdk_llm_fn _complete;
};

// Registers `make` for `Interface` under `id`: nullptr when the app's create returned NULL, so the
// spec's fallback is tried.
template <class Interface, class Make>
int add(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create, Make make) {
    if (!providers || !id || !*id || !create) return 0;
    vasdk_providers_registry(providers).add<Interface>(
        id, std::function<std::shared_ptr<Interface>(const ProviderContext&)>(
                [make, name = std::string(id)](const ProviderContext& context) -> std::shared_ptr<Interface> {
                    auto provider = make(context);
                    if (provider->started()) return provider;
                    spdlog::warn("[vasdk] provider {}: create returned NULL", name);
                    return nullptr;
                }));
    return 1;
}

int rateOf(const ProviderContext& context) { return static_cast<int>(context.audio.sampleRate); }

} // namespace

extern "C" {

void vasdk_sink_text(vasdk_sink* sink, const char* text, int is_final) {
    if (!sink || !text) return;
    va::audio::TranscriptionResultListener* listener = nullptr;
    {
        std::lock_guard<std::mutex> lock(sink->mutex);
        if (sink->capturing) {   // batch: the final is writeFullAudioData's result, partials go nowhere
            if (is_final) {
                sink->final = text;
                sink->captured = true;
            }
            return;
        }
        listener = sink->stt;
    }
    if (!listener) return;
    va::audio::TranscriptionResult result;
    result.text = text;
    result.confidence = 1.0f;
    if (is_final) listener->onFinalTranscription(result);
    else listener->onPartialTranscription(result);
}

void vasdk_sink_audio(vasdk_sink* sink, const short* pcm, size_t samples) {
    if (!sink || !pcm || samples == 0) return;
    va::audio::VoiceSynthesisResultListener* listener = nullptr;
    bool starting = false;
    {
        std::lock_guard<std::mutex> lock(sink->mutex);
        listener = sink->tts;
        starting = !sink->speaking;
        sink->speaking = true;
    }
    if (!listener) return;
    if (starting) listener->onSynthesisStarted();
    listener->onAudioDataReceived(reinterpret_cast<const char*>(pcm), samples * sizeof(short));
}

void vasdk_sink_done(vasdk_sink* sink) {
    if (!sink) return;
    va::audio::VoiceSynthesisResultListener* listener = nullptr;
    bool started = false;
    {
        std::lock_guard<std::mutex> lock(sink->mutex);
        listener = sink->tts;
        started = sink->speaking;
        sink->speaking = false;
    }
    if (!listener) return;
    if (!started) listener->onSynthesisStarted();   // nothing to say: still a whole synthesis
    listener->onSynthesisCompleted();
}

void vasdk_sink_error(vasdk_sink* sink, const char* message) {
    if (!sink) return;
    const std::string error = message ? message : "failed";
    va::audio::TranscriptionResultListener* stt = nullptr;
    va::audio::VoiceSynthesisResultListener* tts = nullptr;
    {
        std::lock_guard<std::mutex> lock(sink->mutex);
        stt = sink->stt;
        tts = sink->tts;
        sink->speaking = false;
    }
    if (stt) stt->onTranscriptionError(error);
    if (tts) tts->onSynthesisError(error);
}

int vasdk_providers_add_vad(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                            vasdk_provider_destroy_fn destroy, vasdk_vad_fn detect, void* user) {
    if (!detect) return 0;
    const Callbacks callbacks{create, destroy, user};
    const std::string name = id ? id : "";
    return add<va::audio::VADProvider>(providers, id, create, [=](const ProviderContext& context) {
        return std::make_shared<AppVad>(name, callbacks, rateOf(context), detect);
    });
}

int vasdk_providers_add_stt(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                            vasdk_provider_destroy_fn destroy, vasdk_stt_event_fn start, vasdk_stt_write_fn write,
                            vasdk_stt_event_fn end_of_speech, vasdk_stt_event_fn stop, void* user) {
    if (!write) return 0;
    const Callbacks callbacks{create, destroy, user};
    const std::string name = id ? id : "";
    return add<va::audio::GenericTranscriptionProvider>(providers, id, create, [=](const ProviderContext& context) {
        auto stt = std::make_shared<AppStt>(name, callbacks, rateOf(context), start, write, end_of_speech, stop);
        stt->setAudioConfiguration(context.audio);
        return stt;
    });
}

int vasdk_providers_add_tts(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                            vasdk_provider_destroy_fn destroy, vasdk_tts_synthesize_fn synthesize, vasdk_tts_cancel_fn cancel,
                            void* user) {
    if (!synthesize) return 0;
    const Callbacks callbacks{create, destroy, user};
    const std::string name = id ? id : "";
    return add<va::audio::GenericVoiceSynthesisProvider>(providers, id, create, [=](const ProviderContext& context) {
        auto tts = std::make_shared<AppTts>(name, callbacks, rateOf(context), synthesize, cancel);
        tts->setAudioConfiguration(context.audio);
        return tts;
    });
}

int vasdk_providers_add_turn(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                             vasdk_provider_destroy_fn destroy, vasdk_turn_fn evaluate, void* user) {
    if (!evaluate) return 0;
    const Callbacks callbacks{create, destroy, user};
    const std::string name = id ? id : "";
    return add<va::audio::TurnDetectionProvider>(providers, id, create, [=](const ProviderContext& context) {
        return std::make_shared<AppTurn>(name, callbacks, context, evaluate);
    });
}

int vasdk_providers_add_wake_word(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                  vasdk_provider_destroy_fn destroy, vasdk_wake_word_fn process, void* user) {
    if (!process) return 0;
    const Callbacks callbacks{create, destroy, user};
    const std::string name = id ? id : "";
    return add<va::audio::WakeWordProvider>(providers, id, create, [=](const ProviderContext& context) {
        return std::make_shared<AppWakeWord>(name, callbacks, rateOf(context), process);
    });
}

int vasdk_providers_add_sentence_encoder(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                         vasdk_provider_destroy_fn destroy, vasdk_encoder_fn encode, void* user) {
    if (!encode) return 0;
    const Callbacks callbacks{create, destroy, user};
    const std::string name = id ? id : "";
    return add<va::nlu::SentenceEncoder>(providers, id, create,
                                         [=](const ProviderContext&) { return std::make_shared<AppEncoder>(name, callbacks, encode); });
}

int vasdk_providers_add_entity_model(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                                     vasdk_provider_destroy_fn destroy, vasdk_entity_model_fn extract, void* user) {
    if (!extract) return 0;
    const Callbacks callbacks{create, destroy, user};
    const std::string name = id ? id : "";
    return add<va::nlu::EntityExtractor>(providers, id, create,
                                         [=](const ProviderContext&) { return std::make_shared<AppEntityModel>(name, callbacks, extract); });
}

int vasdk_providers_add_llm(vasdk_providers* providers, const char* id, vasdk_provider_create_fn create,
                            vasdk_provider_destroy_fn destroy, vasdk_llm_fn complete, vasdk_llm_cancel_fn cancel, void* user) {
    if (!complete) return 0;
    const Callbacks callbacks{create, destroy, user};
    const std::string name = id ? id : "";
    if (!add<va::llm::LLMTransport>(providers, id, create, [=](const ProviderContext& context) {
            return std::make_shared<AppLlm>(name, callbacks, rateOf(context), complete, cancel);
        }))
        return 0;
    return add<va::llm::Completion>(providers, id, create,
                                    [=](const ProviderContext&) { return std::make_shared<AppCompletion>(name, callbacks, complete); });
}

} // extern "C"
