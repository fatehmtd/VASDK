package com.vasdk.android.tools

import org.json.JSONObject
import java.net.URLEncoder

/**
 * get_weather: the current weather for a place, from Open-Meteo (https://open-meteo.com: free, no key;
 * data CC BY 4.0, credited in the result and in THIRD_PARTY_NOTICES.md). The place is looked up with
 * its geocoding API, then its forecast API gives the current conditions.
 */
object WeatherTools {

    val toolNames = setOf("get_weather")

    fun owns(toolName: String) = toolName in toolNames

    /** [httpGet] returns a URL's body, or null when the request fails (WebTools.httpGet in the app). */
    fun handle(toolName: String, argsJson: String, httpGet: (String) -> String?): String = when (toolName) {
        "get_weather" -> currentWeather(argsJson, httpGet)
        else -> JSONObject().put("error", "unknown tool: $toolName").toString()
    }

    private fun currentWeather(argsJson: String, httpGet: (String) -> String?): String {
        val args = JSONObject(argsJson)
        val location = args.optString("location").trim()
        if (location.isEmpty()) return error("say which place: a city, a region or a country")
        val fahrenheit = args.optString("unit").equals("fahrenheit", ignoreCase = true)

        val geocoding = httpGet(
            "https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&format=json&name=" +
                URLEncoder.encode(location, "UTF-8")
        ) ?: return error("the weather service couldn't be reached")
        val place = JSONObject(geocoding).optJSONArray("results")?.optJSONObject(0)
            ?: return error("no place called \"$location\" was found")

        val forecast = httpGet(
            "https://api.open-meteo.com/v1/forecast?latitude=${place.getDouble("latitude")}" +
                "&longitude=${place.getDouble("longitude")}" +
                "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m" +
                "&wind_speed_unit=kmh&timezone=auto" +
                if (fahrenheit) "&temperature_unit=fahrenheit" else ""
        ) ?: return error("the weather service couldn't be reached")
        val current = JSONObject(forecast).optJSONObject("current")
            ?: return error("the weather service sent no current conditions")

        // Words, not symbols: the model repeats them, and the reply is spoken
        val unit = if (fahrenheit) " degrees Fahrenheit" else " degrees Celsius"
        return JSONObject().apply {
            put("location", listOf(place.optString("name"), place.optString("admin1"), place.optString("country"))
                .filter { it.isNotEmpty() }.distinct().joinToString(", "))
            put("conditions", describe(current.optInt("weather_code", -1)))
            put("temperature", "${current.optDouble("temperature_2m").roundToOne()}$unit")
            put("feels_like", "${current.optDouble("apparent_temperature").roundToOne()}$unit")
            put("humidity", "${current.optInt("relative_humidity_2m")} percent")
            put("wind", "${current.optDouble("wind_speed_10m").roundToOne()} kilometres per hour")
            put("observed_at", current.optString("time"))   // local time at the place
            put("source", "Open-Meteo.com")
        }.toString()
    }

    private fun Double.roundToOne() = Math.round(this * 10.0) / 10.0

    private fun error(message: String) = JSONObject().put("error", message).toString()

    // WMO weather interpretation codes, as Open-Meteo documents them
    fun describe(code: Int): String = when (code) {
        0 -> "clear sky"
        1 -> "mainly clear"
        2 -> "partly cloudy"
        3 -> "overcast"
        45, 48 -> "fog"
        51, 53, 55 -> "drizzle"
        56, 57 -> "freezing drizzle"
        61 -> "light rain"
        63 -> "rain"
        65 -> "heavy rain"
        66, 67 -> "freezing rain"
        71 -> "light snow"
        73 -> "snow"
        75 -> "heavy snow"
        77 -> "snow grains"
        80, 81, 82 -> "rain showers"
        85, 86 -> "snow showers"
        95 -> "thunderstorm"
        96, 99 -> "thunderstorm with hail"
        else -> "unknown"
    }
}
