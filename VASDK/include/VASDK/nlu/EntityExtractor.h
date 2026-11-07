#pragma once

#include "vasdk_export.h"

#include <cstddef>
#include <string>
#include <vector>

namespace va {
namespace nlu {

/** An agent's list entity, as an entity model is told about it. */
struct EntityLabel {
    std::string entity;                // its name: "service"
    std::string label;                 // what the model is told it is: Matching.label, else the name ("available time")
    std::string description;           // the type's description
    std::vector<std::string> examples; // some of its values and synonyms, as they're said
};

/** Words a model marked as an entity: indexes into flow::entityWords() of the text. */
struct EntitySpan {
    std::string entity;
    size_t start = 0;
    size_t length = 0;
    float score = 0;   // the model's confidence, 0 to 1
};

/**
 * @brief Finds where entities are said: which words are a "service", which an "available time".
 *        The words are then resolved to the entity's values by EntityCatalog, which, with spans
 *        given, takes misspellings and sound-alikes only inside them. Implementations: a zero-shot
 *        model on device (gliner), a tagger trained from the agent's examples, an LLM.
 *        Thread-safe.
 */
class VASDK_EXPORT EntityExtractor {
public:
    virtual ~EntityExtractor() = default;
    virtual std::string name() const = 0;
    /**
     * Where `labels`' entities are in `words` (flow::entityWords), no two overlapping.
     * `sensitive`: the words may hold a secret answer; extractors that send them off the device
     * (an LLM) return nothing.
     */
    virtual std::vector<EntitySpan> extract(const std::vector<std::string>& words, const std::vector<EntityLabel>& labels,
                                            bool sensitive) const = 0;
};

/** The best-scored spans that don't overlap, in the order said (greedy, highest score first). */
VASDK_EXPORT std::vector<EntitySpan> nonOverlapping(std::vector<EntitySpan> spans);

} // namespace nlu
} // namespace va
