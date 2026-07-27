#pragma once

#include <VASDK/llm/LLMProcessor.h>
#include <vector>
#include <mutex>

namespace va {
namespace llm {

class VASDK_EXPORT AggregatingLLMProcessor : public LLMProcessor {
public:
    explicit AggregatingLLMProcessor(std::shared_ptr<LLMTransport> transport);

    std::string getName() const override { return "AggregatingLLMProcessor"; }

    void processFrame(std::shared_ptr<transport::Frame> frame,
                      pipeline::FlowDirection direction) override;

    void setAggregationTimeout(int timeoutMs) {
        _aggregationTimeoutMs = timeoutMs;
    }

    int getAggregationTimeout() const {
        return _aggregationTimeoutMs;
    }

    void setMaxAggregationCount(size_t maxCount) {
        _maxAggregationCount = maxCount;
    }

    size_t getMaxAggregationCount() const {
        return _maxAggregationCount;
    }

    void flush();

protected:
    void flushAggregatedContent();

private:
    std::string extractAggregatableText(std::shared_ptr<transport::Frame> frame) const;

    std::vector<std::string> _aggregatedTexts;
    uint64_t _lastInputTimestamp = 0;
    int _aggregationTimeoutMs = 2000;
    size_t _maxAggregationCount = 5;
    std::mutex _aggregationMutex;
};

} // namespace llm
} // namespace va
