#pragma once

#include "vasdk_export.h"
#include <VASDK/llm/LLMTransport.h>
#include <VASDK/orchestrator/NLUProvider.h>
#include <orchestrator/training.pb.h>

#include <string>
#include <vector>

namespace va {
namespace orchestrator {

// Milestone 2.2 (docs/reference/requestflow-roadmap.md §4) - the second of
// v1's two NLUProvider implementations (spec §5): a structured classification
// call against a configured va::llm::LLMTransport, proving the interface is
// genuinely swappable now that M2.1's TrainingPhraseNLUProvider exists (spec
// §16: swapping one for the other must require zero changes to
// OrchestratorEngine/OrchestratorFrameProcessor - trivially true today since
// neither is wired into the engine yet; that's M2.3).
//
// "Structured classification call, not a free-form chat turn" (spec §5) is
// implemented as a single request/response round trip: detect() installs a
// system message listing candidate intents plus their TrainingSet phrases as
// few-shot examples (spec §22.2 - the same TrainingSet TrainingPhraseNLUProvider
// reads, so switching providers never means maintaining two example sets) and
// an explicit "respond with exactly this JSON shape" instruction, sends the
// utterance as a TextFrame, and parses the assistant's reply text as
// nlohmann::json (the same JSON library OpenAITransport/AnthropicTransport
// already use - not google::protobuf::util::JsonStringToMessage, which
// resolves google.protobuf.Struct/Value reflectively against the process's
// generated-descriptor pool and is fragile the moment more than one
// statically-linked copy of libprotobuf exists in the same process; plain
// generated accessors like NLUResult.mutable_extracted_parameters() don't
// have that problem). Deliberately NOT built on va::llm::Tool/ToolDispatchProcessor's
// function-calling loop: that machinery is pipeline/frame-mediated and built
// for multi-turn tool use (the right tool for M2.4's generative-node loop),
// which is more machinery than a single synchronous classification call
// needs - LLMTransport::sendFrame() is already a blocking round trip
// end-to-end (HttpTransport::httpPost is synchronous), so a plain structured
// text response is sufficient and keeps this provider self-contained.
//
// Expects a transport dedicated to classification calls: detect() clears the
// transport's history on every call to keep classification stateless and
// contamination-free, so this must not be the same LLMTransport instance
// backing a live conversational loop (e.g. a generative node's llmGenerate
// loop, M2.4) - give this provider its own instance, same as
// va::goal::IntentRouter expects its own attached transport.
//
// Same Phase-4 deferrals as TrainingPhraseNLUProvider (see NLUProvider.h):
// interrupt_intent_names isn't consulted and co_intents is never populated.
class VASDK_EXPORT LLMNLUProvider : public NLUProvider {
public:
    // transport must outlive this provider and must not be shared with a live
    // conversational LLM loop (see class comment). trainingSet supplies the
    // few-shot phrase examples (spec §22.2); an intent with no matching
    // TrainingSet entry is still classifiable by name alone, just less
    // reliably - spec §22.3's own "zero examples" fallback.
    explicit LLMNLUProvider(va::llm::LLMTransport* transport, TrainingSet trainingSet = TrainingSet{});

    bool initialize(const google::protobuf::Struct& config) override;
    void release() override;

    va::orchestrator::NLUResult detect(const std::string& utterance,
                                        const va::orchestrator::NLUContext& context) override;

    // Persistent candidate-set hint (see NLUProvider.h) - used whenever a
    // given detect() call's context.candidate_intent_names is empty.
    void setCandidateIntents(const std::vector<std::string>& names) override;

private:
    va::llm::LLMTransport* _transport;
    TrainingSet _trainingSet;
    std::vector<std::string> _candidateIntents;
    bool _initialized = false;

    const IntentTrainingData* findTrainingData(const std::string& intentName) const;
    std::string buildClassificationPrompt(const std::vector<std::string>& candidateNames) const;
};

} // namespace orchestrator
} // namespace va
