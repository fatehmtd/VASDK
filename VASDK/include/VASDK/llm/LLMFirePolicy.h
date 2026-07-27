#pragma once

#include <string>
#include <memory>

namespace va {
namespace llm {

struct LLMFireContext {
    std::string partialText;
    float       partialConfidence;
    float       turnConfidence;
    int         wordCount;
};

enum class LLMFireDecision {
    FIRE_NOW,
    WAIT_FOR_FINAL,
};

class LLMFirePolicyInterface {
public:
    virtual ~LLMFirePolicyInterface() = default;
    virtual LLMFireDecision evaluate(const LLMFireContext& ctx) const = 0;
    virtual const char*     name() const = 0;
};

class EagerFirePolicy final : public LLMFirePolicyInterface {
public:
    LLMFireDecision evaluate(const LLMFireContext& ctx) const override {
        return ctx.partialText.empty() ? LLMFireDecision::WAIT_FOR_FINAL
                                       : LLMFireDecision::FIRE_NOW;
    }
    const char* name() const override { return "EagerFirePolicy"; }
};

class FinalOnlyFirePolicy final : public LLMFirePolicyInterface {
public:
    LLMFireDecision evaluate(const LLMFireContext& /*ctx*/) const override {
        return LLMFireDecision::WAIT_FOR_FINAL;
    }
    const char* name() const override { return "FinalOnlyFirePolicy"; }
};

class ConfidenceGatedFirePolicy final : public LLMFirePolicyInterface {
public:
    explicit ConfidenceGatedFirePolicy(
        float minTurnConfidence    = 0.7f,
        float minPartialConfidence = 0.85f,
        int   minWordCount         = 3)
        : _minTurnConfidence(minTurnConfidence)
        , _minPartialConfidence(minPartialConfidence)
        , _minWordCount(minWordCount)
    {}

    LLMFireDecision evaluate(const LLMFireContext& ctx) const override {
        if (ctx.partialText.empty())
            return LLMFireDecision::WAIT_FOR_FINAL;
        if (ctx.turnConfidence < _minTurnConfidence)
            return LLMFireDecision::WAIT_FOR_FINAL;
        if (ctx.partialConfidence < _minPartialConfidence)
            return LLMFireDecision::WAIT_FOR_FINAL;
        if (ctx.wordCount < _minWordCount)
            return LLMFireDecision::WAIT_FOR_FINAL;
        return LLMFireDecision::FIRE_NOW;
    }

    const char* name() const override { return "ConfidenceGatedFirePolicy"; }

    float minTurnConfidence()    const { return _minTurnConfidence; }
    float minPartialConfidence() const { return _minPartialConfidence; }
    int   minWordCount()         const { return _minWordCount; }

private:
    float _minTurnConfidence;
    float _minPartialConfidence;
    int   _minWordCount;
};

} // namespace llm
} // namespace va
