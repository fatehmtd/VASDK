#pragma once

#include <VASDK/flow/FlowRegistry.h>

#include <nlohmann/json.hpp>
#include <vector>

namespace garage {

/**
 * The garage agent's steps and envelopes written in C++:
 *   - custom { [type.googleapis.com/garage.BookingReference] { into: "$session.reference" } }:
 *     a reference made from the booking form's values, the same for the same booking.
 *   - envelopes: ["thanks"]: "thanks" on its own gets "You're welcome." and the question again.
 *   - call_tool { name: "save_booking" ... }: keeps the booking (here, in memory) and returns
 *     {"reference": ..., "status": "confirmed"}; a booking without a reference fails.
 */
void registerExtensions(va::flow::FlowRegistry& registry);

/** The bookings save_booking kept, as the JSON objects it was given. */
std::vector<nlohmann::json> savedBookings();

} // namespace garage
