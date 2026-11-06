#include "VASDK/Masking.h"

#include <algorithm>
#include <cctype>

namespace va {

std::string maskValue(const std::string& value) {
    const auto digits = std::count_if(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
    long keep = digits >= 8 ? 4 : 0;   // a phone or card number keeps its last 4 digits, as receipts do
    std::string masked = value;
    for (auto c = masked.rbegin(); c != masked.rend(); ++c) {
        const auto character = static_cast<unsigned char>(*c);
        if (std::isdigit(character) && keep > 0) --keep;
        else if (std::isalnum(character)) *c = '*';
    }
    return masked;
}

std::string redact(std::string text, const std::vector<std::string>& values) {
    std::vector<std::string> longestFirst = values;
    std::sort(longestFirst.begin(), longestFirst.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    for (const auto& value : longestFirst) {
        if (value.size() < 2) continue;
        const std::string masked = maskValue(value);
        for (size_t at = text.find(value); at != std::string::npos; at = text.find(value, at + masked.size()))
            text.replace(at, value.size(), masked);
    }
    return text;
}

} // namespace va
