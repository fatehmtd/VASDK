#include <VASDK/orchestrator/SessionPersistence.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <google/protobuf/text_format.h>
#include <sstream>
#include <spdlog/spdlog.h>

namespace va {
namespace orchestrator {

FileSessionPersistence::FileSessionPersistence(std::string baseDir, bool debugTextSessions)
    : _baseDir(std::move(baseDir))
    , _debugTextSessions(debugTextSessions) {}

std::string FileSessionPersistence::filePath(const std::string& sessionKey) const {
    // Sanitise key - replace path separators to prevent directory traversal,
    // same rule as FileGoalStatePersistence::filePath.
    std::string safe;
    safe.reserve(sessionKey.size());
    for (char c : sessionKey) {
        safe += (c == '/' || c == '\\' || c == ':') ? '_' : c;
    }
    return _baseDir + "/" + safe + (_debugTextSessions ? ".session.textproto" : ".session.pb");
}

bool FileSessionPersistence::save(const std::string& sessionKey, const SessionSnapshot& snapshot) {
    const std::string path = filePath(sessionKey);

    if (_debugTextSessions) {
        std::string text;
        if (!google::protobuf::TextFormat::PrintToString(snapshot, &text)) {
            spdlog::error("[SessionPersistence] Failed to serialize snapshot for '{}' to text format", sessionKey);
            return false;
        }
        std::ofstream f(path);
        if (!f) {
            spdlog::error("[SessionPersistence] Cannot write to '{}'", path);
            return false;
        }
        f << text;
        spdlog::debug("[SessionPersistence] Session saved for '{}' (text)", sessionKey);
        return true;
    }

    std::string bytes;
    if (!snapshot.SerializeToString(&bytes)) {
        spdlog::error("[SessionPersistence] Failed to serialize snapshot for '{}'", sessionKey);
        return false;
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        spdlog::error("[SessionPersistence] Cannot write to '{}'", path);
        return false;
    }
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    spdlog::debug("[SessionPersistence] Session saved for '{}'", sessionKey);
    return true;
}

std::optional<SessionSnapshot> FileSessionPersistence::load(const std::string& sessionKey) {
    const std::string path = filePath(sessionKey);

    if (_debugTextSessions) {
        std::ifstream f(path);
        if (!f) return std::nullopt;
        std::ostringstream buf;
        buf << f.rdbuf();
        SessionSnapshot snap;
        if (!google::protobuf::TextFormat::ParseFromString(buf.str(), &snap)) {
            spdlog::warn("[SessionPersistence] Failed to parse text session for '{}'", sessionKey);
            return std::nullopt;
        }
        spdlog::debug("[SessionPersistence] Session loaded for '{}' (text)", sessionKey);
        return snap;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream buf;
    buf << f.rdbuf();
    SessionSnapshot snap;
    if (!snap.ParseFromString(buf.str())) {
        spdlog::warn("[SessionPersistence] Failed to parse session for '{}'", sessionKey);
        return std::nullopt;
    }
    spdlog::debug("[SessionPersistence] Session loaded for '{}'", sessionKey);
    return snap;
}

bool FileSessionPersistence::clear(const std::string& sessionKey) {
    const std::string path = filePath(sessionKey);
    if (std::remove(path.c_str()) == 0) {
        spdlog::debug("[SessionPersistence] Session cleared for '{}'", sessionKey);
        return true;
    }
    // Already absent counts as cleared (idempotent), matching the
    // save/load/clear contract's spirit - anything else is a real failure.
    return errno == ENOENT;
}

} // namespace orchestrator
} // namespace va
