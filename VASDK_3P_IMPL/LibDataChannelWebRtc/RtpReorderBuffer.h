#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace va {
namespace webrtc {

/**
 * @brief Puts RTP payloads back in sequence order.
 *
 * Holds up to `depth` packets that arrived ahead of a gap; once more are waiting, the gap is
 * given up on and reported lost so the decoder can conceal it. Late and duplicate packets are
 * dropped. Sequence numbers wrap at 16 bits.
 */
class RtpReorderBuffer {
public:
    using Payload = std::vector<std::byte>;

    explicit RtpReorderBuffer(size_t depth = 3) : _depth(depth) {}

    /// @param deliver void(const Payload&) - called for each in-order payload
    /// @param lost    void(int count)      - called for each gap given up on
    template <class Deliver, class Lost>
    void push(uint16_t seq, Payload payload, Deliver&& deliver, Lost&& lost) {
        if (!_started) { _started = true; _next = seq; }

        int16_t ahead = static_cast<int16_t>(seq - _next);
        if (ahead < 0) return;  // late or duplicate
        if (ahead > kResyncDistance) {  // sender restarted its sequence; start over
            _held.clear();
            _next = seq;
        }
        _held.emplace(seq, std::move(payload));

        drain(deliver);
        while (_held.size() > _depth) {
            // Jump to the held packet nearest to the gap and report what was skipped.
            uint16_t nearest = _held.begin()->first;
            for (auto& kv : _held)
                if (static_cast<uint16_t>(kv.first - _next) < static_cast<uint16_t>(nearest - _next))
                    nearest = kv.first;
            lost(static_cast<uint16_t>(nearest - _next));
            _next = nearest;
            drain(deliver);
        }
    }

    void reset() { _started = false; _held.clear(); }

private:
    static constexpr int kResyncDistance = 1000;

    template <class Deliver>
    void drain(Deliver& deliver) {
        for (auto it = _held.find(_next); it != _held.end(); it = _held.find(_next)) {
            deliver(it->second);
            _held.erase(it);
            ++_next;
        }
    }

    size_t _depth;
    bool _started = false;
    uint16_t _next = 0;
    std::map<uint16_t, Payload> _held;
};

} // namespace webrtc
} // namespace va
