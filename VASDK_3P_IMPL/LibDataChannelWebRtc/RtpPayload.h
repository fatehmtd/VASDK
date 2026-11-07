#pragma once

#include <algorithm>
#include <cstddef>

namespace va::thirdparty {

/**
 * @brief Finds the payload of an RTP packet (RFC 3550), skipping CSRCs, header extension and padding.
 *
 * Every length here comes from the remote peer, so each one is bounds-checked.
 * @return false if the packet is malformed or has an empty payload
 */
inline bool rtpPayloadRange(const std::byte* p, size_t size, size_t& begin, size_t& end) {
    if (size < 12) return false;
    auto at = [p](size_t i) { return std::to_integer<size_t>(p[i]); };
    begin = 12 + 4 * (at(0) & 0x0F);  // fixed header + CSRCs
    if (at(0) & 0x10) {               // header extension: 4-byte header + length in 32-bit words
        if (begin + 4 > size) return false;
        begin += 4 + 4 * ((at(begin + 2) << 8) | at(begin + 3));
    }
    end = size;
    if (at(0) & 0x20) end -= std::min(end, at(size - 1));  // last byte counts the padding
    return begin < end;
}

} // namespace va::thirdparty
