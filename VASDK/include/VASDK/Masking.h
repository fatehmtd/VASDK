#pragma once

#include "vasdk_export.h"

#include <string>
#include <vector>

namespace va {

/** "555-123-4567" → "***-***-4567": letters and all but the last 4 digits starred. */
VASDK_EXPORT std::string maskValue(const std::string& value);
/** `text` with each of `values` in it masked. */
VASDK_EXPORT std::string redact(std::string text, const std::vector<std::string>& values);

} // namespace va
