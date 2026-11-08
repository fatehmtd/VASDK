#pragma once

#include <VASDK/vasdk_export.h>
#include <VASDK/engine/Runtime.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace va {

/**
 * @brief Conversations by id over one engine::Runtime, e.g. one session per phone call.
 *
 * The runtime loads models once per concurrent session and reuses them; this class keeps the
 * running sessions so a telephony layer can find a call by id and end it.
 *
 * @code
 * va::engine::Runtime::Options limits;
 * limits.maxSessions = 10;
 * va::SessionManager calls(va::engine::Runtime::create(config, registry, limits));
 * va::engine::Session::Options call;
 * call.id = callId;
 * call.transport = transportForTheCall;
 * if (auto session = calls.createSession(call, &error)) session->start();
 * else rejectCall(error);   // e.g. all sessions busy
 * ...
 * calls.destroySession(callId);   // on hang-up
 * @endcode
 */
class VASDK_EXPORT SessionManager {
public:
    explicit SessionManager(std::shared_ptr<engine::Runtime> runtime);
    ~SessionManager();   // stops every session still running

    /** Creates (not starts) a session; options.id must be set and unused. nullptr, with
     *  `error` set, otherwise or when the runtime is at maxSessions. */
    std::shared_ptr<engine::Session> createSession(engine::Session::Options options, std::string* error = nullptr);
    /** nullptr when there's no session with that id. */
    std::shared_ptr<engine::Session> session(const std::string& id) const;
    /** Stops the session; its providers go back to the runtime once nothing else holds it. */
    void destroySession(const std::string& id);
    size_t activeSessionCount() const;

    engine::Runtime& runtime() { return *_runtime; }

private:
    std::shared_ptr<engine::Runtime> _runtime;
    mutable std::mutex _mutex;
    std::unordered_map<std::string, std::shared_ptr<engine::Session>> _sessions;
};

} // namespace va
