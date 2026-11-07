#include "nlu/EntityExtractor.h"

#include <algorithm>

namespace va {
namespace nlu {

std::vector<EntitySpan> nonOverlapping(std::vector<EntitySpan> spans) {
    std::stable_sort(spans.begin(), spans.end(), [](const EntitySpan& a, const EntitySpan& b) { return a.score > b.score; });
    std::vector<EntitySpan> kept;
    for (auto& span : spans) {
        if (span.length == 0) continue;
        const bool overlaps = std::any_of(kept.begin(), kept.end(), [&](const EntitySpan& other) {
            return span.start < other.start + other.length && other.start < span.start + span.length;
        });
        if (!overlaps) kept.push_back(std::move(span));
    }
    std::sort(kept.begin(), kept.end(), [](const EntitySpan& a, const EntitySpan& b) { return a.start < b.start; });
    return kept;
}

} // namespace nlu
} // namespace va
