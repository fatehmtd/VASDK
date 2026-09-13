#pragma once

#include "vasdk_export.h"
#include <orchestrator/domain.pb.h>

#include <google/protobuf/struct.pb.h>

#include <string>
#include <vector>

namespace va {
namespace orchestrator {

// Milestone 2.1 (docs/reference/requestflow-roadmap.md §4) - spec §5's
// polymorphic NLU interface, same shape as every other provider in the core
// library (initialize/release lifecycle, pure-virtual core, no ML framework
// forced on anyone who doesn't build a given implementation - see e.g.
// va::audio::GenericAudioDSPProvider). v1 ships two implementations behind
// this interface: TrainingPhraseNLUProvider (this milestone, fully local, no
// model) and an LLM-tool-backed provider (M2.2) - proving the local one first
// is the "LLM-optional by construction" claim in spec §2.
//
// detect() is called once per turn by OrchestratorEngine/OrchestratorFrameProcessor
// (M2.3 wires this in - the interface exists a milestone ahead of its caller,
// same "compile the contract before the one caller that needs it" order M1.3
// already used for the phrase-grammar compiler itself). Two call shapes share
// one method: context.expected_parameter_name set means "extract a value for
// this parameter from free text" (slot-filling, spec §9.5); unset means "classify
// an intent among context.candidate_intent_names" (spec §6 step 2). Checking
// context.interrupt_intent_names first (spec §6.2) and populating
// NLUResult.co_intents (spec §6.1) are both Phase 4 concerns per the roadmap's
// §5 table, not required of a conforming provider yet.
class VASDK_EXPORT NLUProvider {
public:
    virtual ~NLUProvider() = default;

    // Implementation-specific setup (e.g. an LLM-backed provider's endpoint/model
    // config). TrainingPhraseNLUProvider takes its TrainingSet via constructor
    // instead - compiled once at construction time, per spec §22.1's "fail
    // loudly, not at match time" rule - so its initialize() has nothing to do.
    virtual bool initialize(const google::protobuf::Struct& config) = 0;

    // Release all resources. Must not be used again without initialize().
    virtual void release() = 0;

    virtual va::orchestrator::NLUResult detect(const std::string& utterance,
                                                const va::orchestrator::NLUContext& context) = 0;

    // Optional: restrict/hint classification scope. Default no-op - a provider
    // that doesn't override this just classifies against whatever it always has.
    virtual void setCandidateIntents(const std::vector<std::string>& names) {}
};

} // namespace orchestrator
} // namespace va
