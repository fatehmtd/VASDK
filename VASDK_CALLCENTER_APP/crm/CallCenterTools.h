#pragma once

#include <VASDK/flow/FlowRegistry.h>

#include <nlohmann/json.hpp>
#include <vector>

namespace callcenter {

/**
 * The call center agent's tools, written in C++: where a real CRM and scheduling system plug in.
 *   - lookup_account {identifier}: an account by phone number (any formatting) or account id:
 *     {"found": true, "account_id", "name", "phone", "tier"}, or {"found": false}.
 *   - schedule_callback {customer_name, phone_number, service_type, vehicle, appointment_date}:
 *     keeps the callback (here, in memory) and returns {"reference": "CB-1234", "status": "scheduled"}.
 */
void registerTools(va::flow::FlowRegistry& registry);

/** The callbacks schedule_callback kept, as the JSON objects it was given. */
std::vector<nlohmann::json> scheduledCallbacks();

} // namespace callcenter
