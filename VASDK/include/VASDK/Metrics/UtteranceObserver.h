#pragma once
#include <cstdint>

namespace va {
namespace metrics {

class UtteranceObserverInterface {
public:
    virtual ~UtteranceObserverInterface() = default;
    virtual void onUtteranceEnd(uint64_t utteranceStartNs) noexcept = 0;
};

} // namespace metrics
} // namespace va
