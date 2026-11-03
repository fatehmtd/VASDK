#include "flow/SlotExtraction.h"

#include "flow/CompiledAgent.h"
#include "goal/GoalState.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace va {
namespace flow {

namespace {

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n.!?") - first + 1);
}

const std::map<std::string, int>& numberWords() {
    static const std::map<std::string, int> words{
        {"zero", 0}, {"oh", 0}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6},
        {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10}, {"eleven", 11}, {"twelve", 12}, {"thirteen", 13},
        {"fourteen", 14}, {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18},
        {"nineteen", 19}, {"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fifty", 50}, {"sixty", 60},
        {"seventy", 70}, {"eighty", 80}, {"ninety", 90}};
    return words;
}

bool isNumber(const std::string& word, bool decimals) {
    if (word.empty()) return false;
    bool digit = false, point = false;
    for (const char c : word) {
        if (std::isdigit(static_cast<unsigned char>(c))) digit = true;
        else if (c == '.' && decimals && !point) point = true;
        else return false;
    }
    return digit;
}

// "twenty five" → 25, "a hundred and two" → 102, "3" → 3: the first number in the words.
std::optional<std::string> extractNumber(const std::string& text, bool decimals) {
    // Digits keep their decimal point: split on spaces only, then strip surrounding punctuation.
    std::vector<std::string> tokens;
    std::string token;
    for (const char c : text + " ") {
        if (std::isspace(static_cast<unsigned char>(c))) {
            while (!token.empty() && !std::isalnum(static_cast<unsigned char>(token.back()))) token.pop_back();
            while (!token.empty() && !std::isalnum(static_cast<unsigned char>(token.front())) && token.front() != '-')
                token.erase(token.begin());
            if (!token.empty()) tokens.push_back(token);
            token.clear();
        } else {
            token += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    for (const auto& word : tokens) {
        const std::string unsignedWord = word[0] == '-' ? word.substr(1) : word;
        if (isNumber(unsignedWord, decimals)) return word;
    }
    long long total = 0, current = 0;
    bool found = false;
    for (const auto& raw : tokens) {
        const auto words = splitWords(raw);   // "twenty-five" → twenty, five
        bool stop = false;
        for (const auto& word : words) {
            const auto known = numberWords().find(word);
            if (known != numberWords().end() && word != "oh") {
                current += known->second;
                found = true;
            } else if (word == "hundred" && (found || current == 0)) {
                current = std::max<long long>(current, 1) * 100;
                found = true;
            } else if (word == "thousand" && (found || current == 0)) {
                total += std::max<long long>(current, 1) * 1000;
                current = 0;
                found = true;
            } else if (found && (word == "and" || word == "a")) {
                continue;
            } else if (word == "a" && !found) {
                continue;   // "a hundred"
            } else if (found) {
                stop = true;
                break;
            }
        }
        if (stop) break;
    }
    if (!found) return std::nullopt;
    return std::to_string(total + current);
}

// Digits written or spoken one by one: "555 one two three, double four" → "55512344".
std::optional<std::string> extractDigits(const std::string& text) {
    std::string digits;
    int repeat = 1;
    for (const auto& word : splitWords(text)) {
        if (word == "double") { repeat = 2; continue; }
        if (word == "triple") { repeat = 3; continue; }
        if (isNumber(word, false)) {
            for (int i = 0; i < repeat; ++i) digits += word;
        } else {
            const auto known = numberWords().find(word);
            if (known != numberWords().end() && known->second < 10)
                digits.append(static_cast<size_t>(repeat), static_cast<char>('0' + known->second));
        }
        repeat = 1;
    }
    if (digits.empty()) return std::nullopt;
    return digits;
}

} // namespace

Value slotValue(const goal::GoalState& state, const std::string& slot) {
    const nlohmann::json json = state.slotValueJson(slot);
    if (json.is_boolean()) return boolValue(json.get<bool>());
    if (json.is_number()) return numberValue(json.get<double>());
    if (json.is_string()) return stringValue(json.get<std::string>());
    if (json.is_null()) return nullValue();
    return stringValue(json.dump());
}

std::optional<bool> extractYesNo(const std::string& text) {
    static const std::map<std::string, bool> answers{
        {"yes", true}, {"yeah", true}, {"yep", true}, {"yup", true}, {"sure", true}, {"correct", true},
        {"right", true}, {"ok", true}, {"okay", true}, {"absolutely", true}, {"definitely", true},
        {"true", true}, {"no", false}, {"nope", false}, {"nah", false}, {"not", false}, {"wrong", false},
        {"incorrect", false}, {"don't", false}, {"false", false}};
    for (const auto& word : splitWords(text)) {
        const auto found = answers.find(word);
        if (found != answers.end()) return found->second;
    }
    return std::nullopt;
}

std::optional<std::string> extractSlotValue(const vasdk::goal::Slot& slot, const std::string& text) {
    using Type = vasdk::goal::Slot;
    switch (slot.type()) {
    case Type::ENUM: {
        const auto words = splitWords(text);
        const std::string* best = nullptr;
        size_t bestLength = 0;
        for (const auto& value : slot.values()) {
            const auto valueWords = splitWords(value);
            if (valueWords.size() > bestLength &&
                std::search(words.begin(), words.end(), valueWords.begin(), valueWords.end()) != words.end()) {
                best = &value;
                bestLength = valueWords.size();
            }
        }
        if (!best) return std::nullopt;
        return *best;
    }
    case Type::INTEGER:
        return extractNumber(text, false);
    case Type::NUMBER:
        return extractNumber(text, true);
    case Type::BOOLEAN: {
        const auto answer = extractYesNo(text);
        if (!answer) return std::nullopt;
        return std::string(*answer ? "yes" : "no");
    }
    case Type::PHONE:
        return extractDigits(text);
    default: {
        auto trimmed = trim(text);
        if (trimmed.empty()) return std::nullopt;
        return trimmed;
    }
    }
}

} // namespace flow
} // namespace va
