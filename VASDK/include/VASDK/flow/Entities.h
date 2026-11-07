#pragma once

#include "vasdk_export.h"
#include "Expression.h"
#include "../nlu/EntityExtractor.h"
#include "../nlu/Language.h"
#include "vasdk/nlu/entity.pb.h"

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace va {
namespace flow {

class FlowRegistry;

/** One entity found in what the user said. */
struct EntityMatch {
    std::string entity;                    // "service", "sys.integer"
    Value value;                           // compared and stored: "oil_change", 25
    std::string spoken;                    // how a reply says it: "an oil change"
    std::string raw;                       // the words heard: "change the oil"
    google::protobuf::Struct attributes;   // a list value's attributes
    size_t start = 0;                      // the words matched, as indexes into entityWords()
    size_t length = 0;
};

/** For one conversation, values replacing a dynamic entity's declared ones: entity name → values. */
using DynamicEntityValues = std::map<std::string, std::vector<vasdk::nlu::EntityValue>>;

/** System entities, usable everywhere a declared one is: "@sys.integer" etc. */
inline constexpr const char* kSystemEntities[] = {"sys.integer", "sys.number",   "sys.phone",    "sys.digits",
                                                  "sys.yes_no",  "sys.date",     "sys.time",     "sys.datetime",
                                                  "sys.duration", "sys.email",   "sys.ordinal"};

/**
 * @brief An agent's entities, checked and ready to find in text.
 *
 * Words are matched whole, ignoring case and punctuation. Between entities, the longest match
 * wins, then pattern over list over derived, then the first declared.
 */
class VASDK_EXPORT EntityCatalog {
public:
    /** No languages: the system entities (@sys.date, @sys.yes_no...) find nothing. */
    EntityCatalog();
    /** System entities read in `languages`, and said in the first. */
    explicit EntityCatalog(std::shared_ptr<const nlu::Languages> languages);

    /** Checks and indexes `types`; each problem goes to `errors` ("entity \"plate\": ..."). Code
     *  entities find their values with `registry`'s extractors. */
    void build(const google::protobuf::RepeatedPtrField<vasdk::nlu::EntityType>& types,
               std::vector<std::string>& errors, std::shared_ptr<const FlowRegistry> registry = nullptr);

    /** A declared entity ("service") or a system one ("sys.integer"), without the @. */
    bool knows(const std::string& name) const;
    /** A declared entity, or nullptr. */
    const vasdk::nlu::EntityType* type(const std::string& name) const;
    /** The languages values are read in. */
    const nlu::Languages& languages() const { return *_languages; }

    /** Every declared entity in `text`, overlaps resolved, in the order said. `spans`: where an
     *  entity model found entities (nlu::EntityExtractor) in entityWords(text); given, a list
     *  entity's misspellings and sound-alikes count only inside its own spans. */
    std::vector<EntityMatch> findAll(const std::string& text, const DynamicEntityValues& dynamic = {},
                                     std::chrono::system_clock::time_point now = std::chrono::system_clock::now(),
                                     const std::vector<nlu::EntitySpan>* spans = nullptr) const;
    /** The longest match of one entity in `text`; the first said on a tie. */
    /** `now`: what dates count from ("tomorrow"). `spans`: as for findAll. */
    std::optional<EntityMatch> find(const std::string& name, const std::string& text,
                                    const DynamicEntityValues& dynamic = {},
                                    std::chrono::system_clock::time_point now = std::chrono::system_clock::now(),
                                    const std::vector<nlu::EntitySpan>* spans = nullptr) const;
    /** The list entities, as an entity model is told about them: what it may find. */
    std::vector<nlu::EntityLabel> modelLabels() const;
    /** A value as if it had been said: a list value's spoken form and attributes. */
    EntityMatch describe(const std::string& name, const Value& value, const DynamicEntityValues& dynamic = {}) const;
    /** Every value of a list entity: this conversation's for a dynamic one; empty for other kinds. */
    std::vector<vasdk::nlu::EntityValue> values(const std::string& name, const DynamicEntityValues& dynamic = {}) const;
    /** Every value of a list entity, as replies say them. */
    std::vector<std::string> spokenValues(const std::string& name, const DynamicEntityValues& dynamic = {}) const;

private:
    std::vector<EntityMatch> matches(const std::string& name, const std::vector<std::string>& words,
                                     const DynamicEntityValues& dynamic, std::chrono::system_clock::time_point now,
                                     const std::vector<nlu::EntitySpan>* spans) const;
    const std::vector<vasdk::nlu::EntityValue>* listValues(const std::string& name, const DynamicEntityValues& dynamic,
                                                           std::vector<vasdk::nlu::EntityValue>& declared) const;
    int kindRank(const std::string& name) const;

    std::vector<std::string> _order;                        // declaration order
    std::map<std::string, vasdk::nlu::EntityType> _types;
    std::map<std::string, std::shared_ptr<std::regex>> _patterns;
    std::map<std::string, std::shared_ptr<const Expression>> _conditions;   // derived entities
    std::shared_ptr<const FlowRegistry> _registry;                          // code entities' extractors
    std::shared_ptr<const nlu::Languages> _languages;
};

/** "Hi, I'm 25." → {"hi", "i'm", "25"}: lower case (nlu::foldedText); a decimal point between digits is kept ("2.5"). */
VASDK_EXPORT std::vector<std::string> entityWords(const std::string& text);

/**
 * What a reference into a match reads: the value itself (its spoken form in a reply, with
 * `spoken`), or the field path[rest]: value, spoken, raw, entity, or an attribute.
 */
VASDK_EXPORT Value entityField(const EntityMatch& match, const Path& path, size_t rest, bool spoken);

} // namespace flow
} // namespace va
