#pragma once

// Pure C++ tools (no JNI round-trip needed)
#include <VASDK/llm/Tool.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <ctime>
#include <random>
#include <string>

//  get_current_datetime 

class DateTimeTool : public va::llm::Tool {
public:
    DateTimeTool() : Tool("get_current_datetime", "Get the current local date and time on the device") {
        onCall([](const nlohmann::json&) -> nlohmann::json {
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            return nlohmann::json{{"datetime", buf}};
        });
    }
};

//  generate_random_number 

class RandomNumberTool : public va::llm::Tool {
public:
    RandomNumberTool() : Tool("generate_random_number", "Generate a random integer between min and max (inclusive)") {
        addParameter({"min", "Minimum value (integer)", "integer", true});
        addParameter({"max", "Maximum value (integer)", "integer", true});
        onCall([](const nlohmann::json& args) -> nlohmann::json {
            int minVal = args.value("min", 1);
            int maxVal = args.value("max", 100);
            if (minVal > maxVal) std::swap(minVal, maxVal);
            std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(minVal, maxVal);
            return nlohmann::json{{"result", dist(rng)}, {"min", minVal}, {"max", maxVal}};
        });
    }
};

//  convert_units 

class UnitConvertTool : public va::llm::Tool {
public:
    UnitConvertTool() : Tool("convert_units",
        "Convert a value between units. Supports: "
        "temperature (celsius/fahrenheit/kelvin), "
        "length (meters/kilometers/miles/feet/inches/centimeters), "
        "weight (kilograms/pounds/grams/ounces)") {
        addParameter({"value",     "The numeric value to convert",                       "number", true});
        addParameter({"from_unit", "Source unit (e.g. celsius, meters, kilograms)",      "string", true});
        addParameter({"to_unit",   "Target unit (e.g. fahrenheit, miles, pounds)",       "string", true});
        onCall([](const nlohmann::json& args) -> nlohmann::json {
            double value = args.value("value", 0.0);
            std::string from = args.value("from_unit", "");
            std::string to   = args.value("to_unit",   "");
            for (auto& c : from) c = static_cast<char>(std::tolower(c));
            for (auto& c : to)   c = static_cast<char>(std::tolower(c));

            double result = 0.0;
            bool   ok     = true;

            // Temperature
            if      ((from == "celsius" || from == "c") && (to == "fahrenheit" || to == "f"))
                result = value * 9.0 / 5.0 + 32.0;
            else if ((from == "fahrenheit" || from == "f") && (to == "celsius" || to == "c"))
                result = (value - 32.0) * 5.0 / 9.0;
            else if ((from == "celsius" || from == "c") && (to == "kelvin" || to == "k"))
                result = value + 273.15;
            else if ((from == "kelvin" || from == "k") && (to == "celsius" || to == "c"))
                result = value - 273.15;
            else if ((from == "fahrenheit" || from == "f") && (to == "kelvin" || to == "k"))
                result = (value - 32.0) * 5.0 / 9.0 + 273.15;
            else if ((from == "kelvin" || from == "k") && (to == "fahrenheit" || to == "f"))
                result = (value - 273.15) * 9.0 / 5.0 + 32.0;

            // Length
            else if ((from == "meters" || from == "m") && (to == "kilometers" || to == "km"))
                result = value / 1000.0;
            else if ((from == "kilometers" || from == "km") && (to == "meters" || to == "m"))
                result = value * 1000.0;
            else if ((from == "meters" || from == "m") && (to == "miles" || to == "mi"))
                result = value / 1609.344;
            else if ((from == "miles" || from == "mi") && (to == "meters" || to == "m"))
                result = value * 1609.344;
            else if ((from == "miles" || from == "mi") && (to == "kilometers" || to == "km"))
                result = value * 1.609344;
            else if ((from == "kilometers" || from == "km") && (to == "miles" || to == "mi"))
                result = value / 1.609344;
            else if ((from == "feet" || from == "ft") && (to == "meters" || to == "m"))
                result = value * 0.3048;
            else if ((from == "meters" || from == "m") && (to == "feet" || to == "ft"))
                result = value / 0.3048;
            else if ((from == "inches" || from == "in") && (to == "centimeters" || to == "cm"))
                result = value * 2.54;
            else if ((from == "centimeters" || from == "cm") && (to == "inches" || to == "in"))
                result = value / 2.54;

            // Weight
            else if ((from == "kilograms" || from == "kg") && (to == "pounds" || to == "lbs" || to == "lb"))
                result = value * 2.20462;
            else if ((from == "pounds" || from == "lbs" || from == "lb") && (to == "kilograms" || to == "kg"))
                result = value / 2.20462;
            else if ((from == "grams" || from == "g") && (to == "kilograms" || to == "kg"))
                result = value / 1000.0;
            else if ((from == "kilograms" || from == "kg") && (to == "grams" || to == "g"))
                result = value * 1000.0;
            else if ((from == "ounces" || from == "oz") && (to == "grams" || to == "g"))
                result = value * 28.3495;
            else if ((from == "grams" || from == "g") && (to == "ounces" || to == "oz"))
                result = value / 28.3495;
            else
                ok = false;

            if (!ok) {
                return nlohmann::json{{"error", "Unsupported conversion: " + from + " to " + to}};
            }
            result = std::round(result * 10000.0) / 10000.0;
            return nlohmann::json{{"value", value}, {"from_unit", from}, {"to_unit", to}, {"result", result}};
        });
    }
};

//  Android bridge tool categories 
// Each header defines tool classes that delegate to Kotlin via JNI bridge.

#include "bridge_tool.h"
// Tier 1
#include "system_tools.h"
#include "media_tools.h"
// Tier 2
#include "notification_tools.h"
#include "communication_tools.h"
#include "location_tools.h"
// Tier 3
#include "web_tools.h"
#include "camera_tools.h"
