#pragma once

#include "vasdk_export.h"
#include <chrono>
#include <string>
#include <unordered_map>

namespace va {
namespace metrics {

/// Per-call KPI data collected by GoalManager and SessionManager.
/// Flushed to MetricsReporter when a session ends via SessionManager::destroySession().
///
/// Prometheus metrics derived from this struct:
///   call_goal_completions_total{intent, goal_name}          counter
///   call_escalations_total{reason, department}              counter
///   call_handle_time_seconds{intent, completed}             histogram
///   call_reprompt_count{goal_name}                          histogram
///   call_slot_fill_attempts{slot_name, slot_type}           histogram
///   call_active_sessions                                    gauge
///   call_stt_pool_utilization                               gauge
struct VASDK_EXPORT CallMetrics {
    std::string callId;
    std::string callerANI;
    std::string calledDNIS;
    std::string intentClassified;
    std::string goalName;

    std::chrono::steady_clock::time_point callStart;
    std::chrono::steady_clock::time_point callEnd;

    bool goalCompleted        = false;
    bool escalated            = false;
    bool resumed              = false; ///< Was a partial-completion resume
    int  repromptCount        = 0;
    int  turnCount            = 0;
    int  confirmationAttempts = 0;

    /// Per-slot fill tracking: slot_name → number of update_slot attempts
    std::unordered_map<std::string, int>  slotFillAttempts;
    /// Per-slot outcome: slot_name → whether the final attempt succeeded
    std::unordered_map<std::string, bool> slotFillSuccess;

    /// Time from call start to first successfully-filled slot
    std::chrono::duration<double> timeToFirstSlot{0};
    /// Time from call start to complete_goal (0 if not completed)
    std::chrono::duration<double> timeToCompletion{0};

    /// Total call duration
    std::chrono::duration<double> handleTime() const {
        auto end = (callEnd == std::chrono::steady_clock::time_point{})
                    ? std::chrono::steady_clock::now()
                    : callEnd;
        return end - callStart;
    }

    void recordSlotAttempt(const std::string& slotName, bool success) {
        slotFillAttempts[slotName]++;
        if (success) slotFillSuccess[slotName] = true;
    }
};

} // namespace metrics
} // namespace va
