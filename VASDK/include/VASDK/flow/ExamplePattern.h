#pragma once

#include <VASDK/vasdk_export.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace va {
namespace flow {

/**
 * @brief Intent examples written as patterns, each standing for every sentence it spells.
 *
 *   (a|b|c)   one of them: "(turn|switch) on"; an empty one makes it optional: "(the|)"
 *   [x]       optional: "turn on [the] light"; next to a word, part of it: "light[s]"
 *   ${name}   one of a phrase set's phrases, each a pattern itself
 *   \c        the character itself: \( \) \[ \] \| \$ \\
 *
 * Expanded when an agent loads (CompiledAgent), so what understands the words afterwards sees
 * plain examples. Spaces are collapsed, empty sentences dropped, repeats kept once, in order.
 */

/** One scope a ${name} is looked up in: an intent's sets, a file's, or a pack the file uses. */
struct PhraseScope {
    enum class Kind { Intent, File, Pack };
    Kind kind = Kind::File;
    std::string name;    // a pack's name, its sets' prefix: ${devices.on}; empty for the others
    std::string where;   // for messages: "intent \"lights_on\"", "agents/phone/agent.txtpb", "pack \"devices\""
    std::map<std::string, std::vector<std::string>> sets;
};

/** How many sentences one example (or one set) may spell. */
inline constexpr size_t kMaxExampleExpansions = 256;

/**
 * @brief Expands one file's examples: its phrase sets and the packs it uses are the scopes.
 *
 * A ${name} in an example is looked up nearest first: the intent's own sets (passed to expand),
 * the file's, then the packs (${pack.name} names one; an unqualified name in two packs is an
 * error). A set's phrases see the scopes from its own outwards, and a pack's only that pack.
 * Sets that refer to each other in a loop are an error.
 */
class VASDK_EXPORT ExampleExpander {
public:
    /** `scopes`: the file's, then the packs it uses. */
    explicit ExampleExpander(std::vector<std::shared_ptr<const PhraseScope>> scopes,
                             size_t limit = kMaxExampleExpansions);

    /** Every sentence `pattern` spells, `intentSets` nearest (nullptr: none); nullopt and `error`
     *  (with the column) for a mistake. */
    std::optional<std::vector<std::string>> expand(const std::string& pattern, const PhraseScope* intentSets,
                                                   std::string* error);

    /** Whether a set of `scope` was used by what was expanded so far. */
    bool used(const PhraseScope& scope, const std::string& name) const;
    bool usedAny(const PhraseScope& scope) const;

    /** Notes for each set that hides one of the same name farther out ("intent \"x\": its phrase set
     *  \"on\" hides the file's"); `intentSets` may be nullptr. */
    std::vector<std::string> hidingNotes(const PhraseScope* intentSets) const;

private:
    using Chain = std::vector<const PhraseScope*>;
    struct Found {
        size_t index;
        const std::string* name;
        const std::vector<std::string>* phrases;
    };
    Found find(const Chain& chain, const std::string& reference, size_t from) const;
    std::vector<std::string> spellSet(const Chain& chain, const Found& found);
    std::vector<std::string> spell(const Chain& chain, const std::string& text, size_t from);

    std::vector<std::shared_ptr<const PhraseScope>> _scopes;
    size_t _limit;
    std::map<std::pair<const PhraseScope*, std::string>, std::vector<std::string>> _spelled;   // a set's sentences, once
    std::vector<std::pair<const PhraseScope*, std::string>> _expanding;                        // the sets being spelled: loops
    std::set<std::pair<const PhraseScope*, std::string>> _used;
};

/** A pattern without phrase sets: (a|b), [x], escapes. For language packs' phrases. */
VASDK_EXPORT std::optional<std::vector<std::string>> expandPattern(const std::string& pattern, std::string* error,
                                                                   size_t limit = kMaxExampleExpansions);

/** A name a set or a pack may have: letters, digits and _, not starting with a digit. */
VASDK_EXPORT bool isPhraseSetName(const std::string& name);

/** At most `most` of `examples`, spread evenly from the first: what an intent shows a sentence
 *  encoder (64) or an LLM's prompt (8) when its patterns spell many. */
VASDK_EXPORT std::vector<std::string> sampleExamples(const std::vector<std::string>& examples, size_t most);

/** A word of an example that stands for others, when it's a word of its own: `*` any number of
 *  words (none too), `+` one or more, `_` exactly one. Inside a word ("@fan_level") it's itself. */
VASDK_EXPORT bool isWildcard(const std::string& word);

/** The example without its wildcards, for what reads examples as words rather than matching them:
 *  a sentence encoder, an LLM's prompt, speech hints. "turn _ light on" → "turn light on". */
VASDK_EXPORT std::string withoutWildcards(const std::string& example);

/** At most `most` of `examples` (sampleExamples), without their wildcards, each once. */
VASDK_EXPORT std::vector<std::string> plainExamples(const std::vector<std::string>& examples, size_t most);

/** Most examples per intent a sentence encoder embeds, and an LLM is shown. */
inline constexpr size_t kMostEncodedExamples = 64;
inline constexpr size_t kMostPromptedExamples = 8;

} // namespace flow
} // namespace va
