#include <VASDK/llm/AggregatingLLMProcessor.h>

namespace va {
namespace llm {

AggregatingLLMProcessor::AggregatingLLMProcessor(std::shared_ptr<LLMTransport> transport)
    : LLMProcessor(transport) {
}

void AggregatingLLMProcessor::processFrame(std::shared_ptr<transport::Frame> frame,
                                            pipeline::FlowDirection direction) {
    if (!frame) {
        return;
    }

    if (direction != pipeline::FlowDirection::DOWNSTREAM || !shouldProcess(frame)) {
        if (direction == pipeline::FlowDirection::DOWNSTREAM) {
            pushDownstream(frame);
        } else {
            pushUpstream(frame);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(_aggregationMutex);

    std::string text = extractAggregatableText(frame);
    if (text.empty()) {
        return;
    }

    _aggregatedTexts.push_back(text);
    uint64_t currentTime = transport::Frame::getCurrentTimestamp();

    bool shouldFlush = false;

    if (_aggregatedTexts.size() >= _maxAggregationCount) {
        shouldFlush = true;
    }

    if (_lastInputTimestamp > 0) {
        uint64_t elapsedMs = (currentTime - _lastInputTimestamp) / 1000;
        if (elapsedMs >= static_cast<uint64_t>(_aggregationTimeoutMs)) {
            shouldFlush = true;
        }
    }

    _lastInputTimestamp = currentTime;

    if (shouldFlush) {
        flushAggregatedContent();
    }
}

std::string AggregatingLLMProcessor::extractAggregatableText(std::shared_ptr<transport::Frame> frame) const {
    switch (frame->getType()) {
    case transport::FrameType::TEXT:
        if (auto textFrame = std::dynamic_pointer_cast<transport::TextFrame>(frame)) {
            return textFrame->text;
        }
        return "";
    case transport::FrameType::TRANSCRIPTION: {
        auto transcriptionFrame = std::dynamic_pointer_cast<transport::TranscriptionFrame>(frame);
        if (transcriptionFrame && transcriptionFrame->isFinal) {
            return transcriptionFrame->transcriptionResult.text;
        }
        return "";
    }
    default:
        return "";
    }
}

void AggregatingLLMProcessor::flush() {
    std::lock_guard<std::mutex> lock(_aggregationMutex);
    flushAggregatedContent();
}

void AggregatingLLMProcessor::flushAggregatedContent() {
    if (_aggregatedTexts.empty()) {
        return;
    }

    std::string combinedText;
    for (size_t i = 0; i < _aggregatedTexts.size(); ++i) {
        if (i > 0) {
            combinedText += " ";
        }
        combinedText += _aggregatedTexts[i];
    }

    auto textFrame = std::make_shared<transport::TextFrame>();
    textFrame->text = combinedText;
    textFrame->timestamp = transport::Frame::getCurrentTimestamp();
    textFrame->sequenceId = 0;
    textFrame->frameId = "aggregated";

    LLMProcessor::processDownstream(textFrame);

    _aggregatedTexts.clear();
    _lastInputTimestamp = 0;
}

} // namespace llm
} // namespace va
