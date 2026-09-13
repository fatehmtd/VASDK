#pragma once

#include "vasdk_export.h"
#include <VASDK/FrameProcessor.h>
#include <VASDK/orchestrator/OrchestratorEngine.h>

#include <memory>
#include <string>

namespace va {
namespace orchestrator {

// Milestone 1.6 (docs/reference/requestflow-roadmap.md §3) - wraps
// OrchestratorEngine in the va::pipeline::FrameProcessor contract (spec §13),
// scoped exactly like the engine it drives: event/condition routes only, four
// action kinds, no NLU.
//
// Session start: spec §13's own sketch never shows a "start the session" call
// on OrchestratorEngine/OrchestratorFrameProcessor, and this milestone still
// needs one. TurnStateFrame is already listed among the frame types this
// processor consumes, so a TurnStateFrame{actor=SYSTEM, state=START} is the
// signal this processor uses to call OrchestratorEngine::start() - reusing a
// frame type spec §13 already put in scope here, rather than inventing a new
// one for a gap the public API sketch leaves open.
class VASDK_EXPORT OrchestratorFrameProcessor : public va::pipeline::FrameProcessor {
public:
    explicit OrchestratorFrameProcessor(std::shared_ptr<OrchestratorEngine> engine);

    std::string getName() const override { return "OrchestratorFrameProcessor"; }

    // consumes: TranscriptionFrame (isFinal), TurnStateFrame{START}, ControlFrame::INTERRUPT (see below)
    // emits: TextFrame downstream (say action text)
    void processFrame(std::shared_ptr<va::transport::Frame> frame,
                       va::pipeline::FlowDirection direction) override;

private:
    std::shared_ptr<OrchestratorEngine> _engine;
};

} // namespace orchestrator
} // namespace va
