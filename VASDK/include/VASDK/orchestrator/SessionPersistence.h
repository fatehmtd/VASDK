#pragma once

#include "vasdk_export.h"
#include <orchestrator/domain.pb.h>

#include <optional>
#include <string>

namespace va {
namespace orchestrator {

// Milestone 1.7 (docs/reference/requestflow-roadmap.md §3), spec §10 - same
// three-method contract as va::goal::GoalStatePersistence, retargeted at the
// proto SessionSnapshot (spec §4) instead of a bare nlohmann::json blob,
// since the orchestrator's runtime state is already a proto message with no
// need for an intermediate JSON representation.
class VASDK_EXPORT SessionPersistence {
public:
    virtual ~SessionPersistence() = default;

    virtual bool save(const std::string& sessionKey, const SessionSnapshot& snapshot) = 0;
    virtual std::optional<SessionSnapshot> load(const std::string& sessionKey) = 0;
    virtual bool clear(const std::string& sessionKey) = 0;
};

// Spec §10 - one file per session key, explicitly dev/test-grade like its
// goal/ counterpart (FileGoalStatePersistence). Binary proto
// (SessionSnapshot::SerializeToString()) by default; construct with
// debugTextSessions = true to switch to TextFormat::PrintToString() instead,
// trading a few bytes for a human-readable session file during development -
// spec §10's "a --debug-text-sessions-style engine option" note, expressed
// here as a constructor flag rather than a global engine option since
// nothing else in M1.7 needs it to be engine-wide.
class VASDK_EXPORT FileSessionPersistence : public SessionPersistence {
public:
    explicit FileSessionPersistence(std::string baseDir, bool debugTextSessions = false);

    bool save(const std::string& sessionKey, const SessionSnapshot& snapshot) override;
    std::optional<SessionSnapshot> load(const std::string& sessionKey) override;
    bool clear(const std::string& sessionKey) override;

private:
    std::string _baseDir;
    bool _debugTextSessions;

    std::string filePath(const std::string& sessionKey) const;
};

} // namespace orchestrator
} // namespace va
