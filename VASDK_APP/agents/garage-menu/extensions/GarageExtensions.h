#pragma once

#include <VASDK/flow/FlowRegistry.h>

namespace garage {

/**
 * The garage agent's steps and envelopes written in C++:
 *   - custom { [type.googleapis.com/garage.BookingReference] { into: "$session.reference" } }:
 *     a reference made from the booking form's values, the same for the same booking.
 *   - envelopes: ["thanks"]: "thanks" on its own gets "You're welcome." and the question again.
 */
void registerExtensions(va::flow::FlowRegistry& registry);

} // namespace garage
