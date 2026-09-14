#pragma once

#include "vasdk_export.h"

#include <google/protobuf/struct.pb.h>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace va {
namespace orchestrator {

// Milestone 3.4 - a distinguishable subtype of the "field not present"
// std::runtime_error lookupField() already threw as of M1.4. Every existing
// catch (const std::runtime_error&) site keeps working unchanged (this IS
// one); the one new catch (const FieldNotPresentError&) site
// (OrchestratorEngine::evaluateRoutesOnce()'s condition-route loop) can now
// tell "this route's condition references a session value that simply isn't
// set yet" - the ordinary, expected state of an unguarded condition_cel on a
// node whose own node_enter cascade runs before step 2 has filled anything,
// e.g. spec §14/§19.1's own urgent_to_triage: visit_reason == "urgent" - apart
// from a genuine author bug (unknown namespace, a type mismatch), which still
// propagates and crashes loudly, unchanged.
class VASDK_EXPORT FieldNotPresentError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Milestone 1.4 (docs/reference/requestflow-roadmap.md §3) - a standalone
// hand-rolled parser/evaluator for spec §8's expression language:
// condition_cel/value_cel/args_cel. Not cel-cpp (dropped as a dependency,
// Draft v2.10) - a small, closed grammar instead: arithmetic (+ - * /),
// comparisons (== != > >= < <=), boolean logic (&& || !), has(), one string
// method (.contains()), and dotted field references. See spec §8's grammar
// table for the exact, closed set this implements.
class VASDK_EXPORT ExpressionEvaluator {
public:
    // Compiles `text` once. Throws std::runtime_error - with the offending
    // expression text and a specific reason - on malformed syntax: unbalanced
    // parens, an unterminated string, an unexpected token, or trailing
    // garbage after a complete expression. Spec §8's "fail loudly at load
    // time, not at evaluation" rule.
    explicit ExpressionEvaluator(const std::string& text);

    // Namespaced lookups spec §8/§8.1/§8.2 define. `parameters` covers both
    // session Parameters and the well-known counters (turnCount,
    // repromptCount, sessionElapsedMs, speaker_id) since both are referenced
    // as bare names - the activation doesn't need to distinguish them.
    // entity.* is not supported here - see requestflow-roadmap.md's M1.4
    // entry for why (it resolves at compile time against an EntityDictionary,
    // not a runtime namespace lookup, which is a separate integration).
    struct Activation {
        std::map<std::string, google::protobuf::Value> parameters;
        std::map<std::string, google::protobuf::Value> nlu;
        std::map<std::string, google::protobuf::Value> context;
        std::map<std::string, google::protobuf::Value> shared;
        std::map<std::string, google::protobuf::Value> event;
    };

    // Evaluates the compiled expression against `activation`. Throws
    // FieldNotPresentError (a std::runtime_error subtype, see above) at
    // evaluation time for a reference to a field that isn't present (unless
    // guarded by has()); throws plain std::runtime_error for a type mismatch
    // (e.g. adding a string to a number) or an unknown namespace - this
    // evaluator does not static-type-check against a declared environment the
    // way real CEL does; that's a documented scope reduction, not an
    // oversight.
    google::protobuf::Value evaluate(const Activation& activation) const;

private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};

} // namespace orchestrator
} // namespace va
