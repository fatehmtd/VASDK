#pragma once

#include <VASDK/vasdk_export.h>
#include "vasdk/engine/engine_config.pb.h"
#include "vasdk/engine/events.pb.h"

#include <cstdint>
#include <memory>
#include <string>

namespace va {
namespace engine {

class Session;

/**
 * @brief Watch, and in CONTROL mode drive, a running session over HTTP (engine config
 *        remote_control): every event as Server-Sent Events on GET /events, commands on
 *        POST /command, and a live trace page on GET /.
 *
 * Every request needs the config's token, as "Authorization: Bearer <token>" or ?token=<token>;
 * a wrong one gets 401 and nothing else. In OBSERVE mode commands get 403. It binds to
 * 127.0.0.1 unless the config says otherwise. Session::start starts one when the config
 * enables it; destroy it before the session.
 *
 * @code
 * // remote_control { enabled: true token: "${VASDK_RC_TOKEN}" mode: CONTROL }
 * //   curl -N -H "Authorization: Bearer $VASDK_RC_TOKEN" http://127.0.0.1:8765/events
 * //   curl -H "Authorization: Bearer $VASDK_RC_TOKEN" -d '{"injectText": "book"}' http://127.0.0.1:8765/command
 * @endcode
 */
class VASDK_EXPORT RemoteControl {
public:
    /** Listening once it returns; nullptr, with `error` set, without a token, on a bad bind, or a port in use. */
    static std::unique_ptr<RemoteControl> start(Session& session, const vasdk::engine::RemoteControl& config,
                                                std::string* error = nullptr);
    ~RemoteControl();
    RemoteControl(const RemoteControl&) = delete;
    RemoteControl& operator=(const RemoteControl&) = delete;

    /** The port it listens on (the one picked, for port 0). */
    uint16_t port() const;
    /** Does what a command asks, as POST /command does; false, with `error` set, in OBSERVE mode or for an empty one. */
    bool handle(const vasdk::engine::Command& command, std::string* error = nullptr);

private:
    struct State;
    explicit RemoteControl(std::unique_ptr<State> state);
    std::unique_ptr<State> _state;
};

} // namespace engine
} // namespace va
