#include <VASDK/orchestrator/ParameterValidator.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <google/protobuf/util/json_util.h>
#include <limits>
#include <stdexcept>

namespace va {
namespace orchestrator {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string stripNonDigits(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) out += c;
    }
    return out;
}

std::string todayISO() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

bool isValidISODate(const std::string& s) {
    if (s.size() != 10) return false;
    if (s[4] != '-' || s[7] != '-') return false;
    for (int i : {0,1,2,3,5,6,8,9}) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool isValidISOTime(const std::string& s) {
    // HH:MM or H:MM
    if (s.size() < 4 || s.size() > 5) return false;
    auto colon = s.find(':');
    if (colon == std::string::npos) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == colon) continue;
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

double constraintNumber(const Parameter& parameter, const std::string& key, double defaultValue) {
    auto it = parameter.constraints().fields().find(key);
    if (it == parameter.constraints().fields().end()) return defaultValue;
    return it->second.number_value();
}

bool constraintBool(const Parameter& parameter, const std::string& key, bool defaultValue) {
    auto it = parameter.constraints().fields().find(key);
    if (it == parameter.constraints().fields().end()) return defaultValue;
    return it->second.bool_value();
}

std::vector<std::string> constraintStringList(const Parameter& parameter, const std::string& key) {
    std::vector<std::string> out;
    auto it = parameter.constraints().fields().find(key);
    if (it == parameter.constraints().fields().end()) return out;
    for (const auto& v : it->second.list_value().values()) {
        out.push_back(v.string_value());
    }
    return out;
}

google::protobuf::Value stringValue(const std::string& s) {
    google::protobuf::Value v;
    v.set_string_value(s);
    return v;
}

google::protobuf::Value numberValue(double d) {
    google::protobuf::Value v;
    v.set_number_value(d);
    return v;
}

google::protobuf::Value boolValue(bool b) {
    google::protobuf::Value v;
    v.set_bool_value(b);
    return v;
}

} // namespace

void ParameterValidator::registerValidator(const std::string& name, CustomValidator fn) {
    _customValidators[name] = std::move(fn);
}

ParameterValidationResult ParameterValidator::validateAndNormalize(const Parameter& parameter,
                                                                     const std::string& raw) const {
    if (!parameter.validator_ref().empty()) {
        auto it = _customValidators.find(parameter.validator_ref());
        if (it == _customValidators.end()) {
            throw std::runtime_error("parameter '" + parameter.name() + "' names validator_ref '"
                                      + parameter.validator_ref() + "' which is not registered");
        }
        return it->second(stringValue(raw));
    }

    switch (parameter.type()) {

    case Parameter::STRING:
        return {true, stringValue(raw), ""};

    case Parameter::PHONE: {
        std::string digits = stripNonDigits(raw);
        if (digits.size() == 11 && digits[0] == '1') digits = digits.substr(1);
        if (digits.size() == 10) {
            std::string formatted = digits.substr(0,3) + "-" + digits.substr(3,3) + "-" + digits.substr(6);
            return {true, stringValue(formatted), ""};
        }
        if (digits.size() == 7) {
            std::string formatted = digits.substr(0,3) + "-" + digits.substr(3);
            return {true, stringValue(formatted), ""};
        }
        return {false, {}, "phone number must be 7 or 10 digits (got " + std::to_string(digits.size()) + ")"};
    }

    case Parameter::EMAIL: {
        auto at = raw.find('@');
        if (at == std::string::npos || at == 0)
            return {false, {}, "email must contain '@'"};
        auto dot = raw.find('.', at + 1);
        if (dot == std::string::npos || dot == raw.size() - 1)
            return {false, {}, "email must have a domain (e.g. user@example.com)"};
        return {true, stringValue(raw), ""};
    }

    case Parameter::DATE: {
        if (!isValidISODate(raw))
            return {false, {}, "date must be in ISO 8601 format YYYY-MM-DD (got '" + raw + "')"};
        if (constraintBool(parameter, "mustBeFuture", false) && raw <= todayISO())
            return {false, {}, "date must be in the future (got '" + raw + "')"};
        return {true, stringValue(raw), ""};
    }

    case Parameter::TIME: {
        std::string t = raw;
        if (!isValidISOTime(t))
            return {false, {}, "time must be in HH:MM format (got '" + raw + "')"};
        if (t.size() == 4) t = "0" + t; // H:MM -> HH:MM
        return {true, stringValue(t), ""};
    }

    case Parameter::DATETIME: {
        if (raw.empty())
            return {false, {}, "datetime must be a valid ISO 8601 datetime"};
        if (constraintBool(parameter, "mustBeFuture", false)) {
            std::string datePrefix = raw.size() >= 10 ? raw.substr(0, 10) : raw;
            if (datePrefix <= todayISO())
                return {false, {}, "datetime must be in the future (got '" + raw + "')"};
        }
        return {true, stringValue(raw), ""};
    }

    case Parameter::NUMBER: {
        try {
            // std::stod ignores trailing non-numeric characters (e.g. "12abc"
            // parses as 12) - preserved as-is from GoalState::validateAndNormalize,
            // this is a straight port, not a bug fix.
            double v = std::stod(raw);
            double lo = constraintNumber(parameter, "min", -std::numeric_limits<double>::infinity());
            double hi = constraintNumber(parameter, "max", std::numeric_limits<double>::infinity());
            if (v < lo || v > hi) {
                return {false, {}, "value " + raw + " is outside allowed range ["
                    + std::to_string(lo) + ", " + std::to_string(hi) + "]"};
            }
            return {true, numberValue(v), ""};
        } catch (...) {
            return {false, {}, "'" + raw + "' is not a valid number"};
        }
    }

    case Parameter::INTEGER: {
        try {
            long long v = std::stoll(raw);
            double lo = constraintNumber(parameter, "min", -std::numeric_limits<double>::infinity());
            double hi = constraintNumber(parameter, "max", std::numeric_limits<double>::infinity());
            if (static_cast<double>(v) < lo || static_cast<double>(v) > hi) {
                return {false, {}, "value " + raw + " is outside allowed range"};
            }
            // google::protobuf::Value has no distinct integer kind - normalizes
            // to the same NUMBER_VALUE double as NUMBER, matching the
            // convention M1.3/M1.4 already use for captured numbers.
            return {true, numberValue(static_cast<double>(v)), ""};
        } catch (...) {
            return {false, {}, "'" + raw + "' is not a valid integer"};
        }
    }

    case Parameter::BOOLEAN: {
        std::string lo = toLower(raw);
        if (lo == "yes" || lo == "true"  || lo == "1" || lo == "on")
            return {true, boolValue(true), ""};
        if (lo == "no"  || lo == "false" || lo == "0" || lo == "off")
            return {true, boolValue(false), ""};
        return {false, {}, "'" + raw + "' is not a boolean (yes/no/true/false)"};
    }

    case Parameter::ENUM: {
        std::string lo = toLower(raw);
        auto allowed = constraintStringList(parameter, "allowedValues");
        for (const auto& v : allowed) {
            if (toLower(v) == lo) return {true, stringValue(v), ""};
        }
        std::string allowedStr;
        for (size_t i = 0; i < allowed.size(); ++i) {
            if (i) allowedStr += ", ";
            allowedStr += allowed[i];
        }
        return {false, {}, "'" + raw + "' is not valid - must be one of: " + allowedStr};
    }

    case Parameter::OBJECT: {
        google::protobuf::Value parsed;
        auto status = google::protobuf::util::JsonStringToMessage(raw, &parsed);
        if (!status.ok()) {
            return {false, {}, "parameter '" + parameter.name() + "' is an OBJECT type - '"
                + raw + "' is not valid JSON"};
        }
        return validateObject(parameter, parsed);
    }

    case Parameter::COMPOSITE:
        return {false, {}, "use dot notation (e.g. '" + parameter.name() + ".field') for COMPOSITE parameters"};

    default:
        return {false, {}, "unknown parameter type"};
    }
}

ParameterValidationResult ParameterValidator::validateObject(const Parameter& parameter,
                                                               const google::protobuf::Value& obj) const {
    if (!parameter.validator_ref().empty()) {
        auto it = _customValidators.find(parameter.validator_ref());
        if (it == _customValidators.end()) {
            throw std::runtime_error("parameter '" + parameter.name() + "' names validator_ref '"
                                      + parameter.validator_ref() + "' which is not registered");
        }
        return it->second(obj);
    }

    if (obj.kind_case() != google::protobuf::Value::kStructValue)
        return {false, {}, "expected a JSON object for parameter '" + parameter.name() + "'"};

    const auto& fields = obj.struct_value().fields();

    auto schemaIt = parameter.constraints().fields().find("objectSchema");
    if (schemaIt != parameter.constraints().fields().end()) {
        for (const auto& fieldSpec : schemaIt->second.list_value().values()) {
            const auto& specFields = fieldSpec.struct_value().fields();

            auto nameIt = specFields.find("name");
            if (nameIt == specFields.end()) continue;
            const std::string& fieldName = nameIt->second.string_value();

            bool required = true;
            auto reqIt = specFields.find("required");
            if (reqIt != specFields.end()) required = reqIt->second.bool_value();
            if (!required) continue;

            auto valIt = fields.find(fieldName);
            if (valIt == fields.end())
                return {false, {}, "missing required field '" + fieldName + "' in parameter '" + parameter.name() + "'"};

            std::string jsonType = "string";
            auto typeIt = specFields.find("jsonType");
            if (typeIt != specFields.end()) jsonType = typeIt->second.string_value();

            const auto& val = valIt->second;
            bool typeOk = false;
            if      (jsonType == "string")  typeOk = val.kind_case() == google::protobuf::Value::kStringValue;
            else if (jsonType == "number")  typeOk = val.kind_case() == google::protobuf::Value::kNumberValue;
            else if (jsonType == "integer") typeOk = val.kind_case() == google::protobuf::Value::kNumberValue
                                                      && std::floor(val.number_value()) == val.number_value();
            else if (jsonType == "boolean") typeOk = val.kind_case() == google::protobuf::Value::kBoolValue;
            else                             typeOk = true; // unknown jsonType - accept, matches GoalState::validateObject

            if (!typeOk)
                return {false, {}, "field '" + fieldName + "' must be " + jsonType
                    + " in parameter '" + parameter.name() + "'"};
        }
    }

    return {true, obj, ""};
}

bool ParameterValidator::isRequired(const Parameter& parameter, const ExpressionEvaluator::Activation& activation) {
    if (!parameter.required()) return false;
    if (parameter.condition_cel().empty()) return true;

    ExpressionEvaluator evaluator(parameter.condition_cel());
    google::protobuf::Value result = evaluator.evaluate(activation);
    if (result.kind_case() != google::protobuf::Value::kBoolValue) {
        throw std::runtime_error("condition_cel for parameter '" + parameter.name()
                                  + "' must evaluate to a boolean");
    }
    return result.bool_value();
}

} // namespace orchestrator
} // namespace va
