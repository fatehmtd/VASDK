package com.vasdk.android

import com.vasdk.android.tools.WeatherTools
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * get_weather on Open-Meteo's answers: the place is looked up, its current weather fetched and
 * described; Fahrenheit on request; an unknown place, no network and no place given are errors.
 */
class WeatherToolsTest {
    private val requested = mutableListOf<String>()

    private val tokyo = """{"results":[{"name":"Tokyo","latitude":35.6895,"longitude":139.69171,"country":"Japan","admin1":"Tokyo"}]}"""
    private val forecast = """{"current":{"time":"2026-11-15T19:00","temperature_2m":14.26,"apparent_temperature":12.94,
        "relative_humidity_2m":60,"weather_code":2,"wind_speed_10m":9.4}}"""

    // Answers geocoding with [geocoding] and the forecast with [current]; null: the request failed.
    private fun web(geocoding: String?, current: String?): (String) -> String? = { url ->
        requested += url
        if (url.contains("geocoding-api")) geocoding else current
    }

    private fun weather(args: String, http: (String) -> String?) = JSONObject(WeatherTools.handle("get_weather", args, http))

    @Test
    fun currentWeatherForAPlace() {
        val result = weather("""{"location":"Tokyo"}""", web(tokyo, forecast))
        assertEquals("Tokyo, Japan", result.getString("location"))
        assertEquals("partly cloudy", result.getString("conditions"))
        assertEquals("14.3 degrees Celsius", result.getString("temperature"))
        assertEquals("12.9 degrees Celsius", result.getString("feels_like"))
        assertEquals("60 percent", result.getString("humidity"))
        assertEquals("9.4 kilometres per hour", result.getString("wind"))
        assertEquals("Open-Meteo.com", result.getString("source"))
        assertTrue("the place looked up by name", requested[0].contains("name=Tokyo"))
        assertTrue("its forecast fetched by position", requested[1].contains("latitude=35.6895") && requested[1].contains("longitude=139.69171"))
    }

    @Test
    fun fahrenheitOnRequest() {
        val result = weather("""{"location":"Tokyo","unit":"fahrenheit"}""", web(tokyo, forecast))
        assertTrue(requested[1].contains("temperature_unit=fahrenheit"))
        assertEquals("14.3 degrees Fahrenheit", result.getString("temperature"))
    }

    @Test
    fun anUnknownPlaceIsAnError() {
        val result = weather("""{"location":"Atlantis"}""", web("""{"generationtime_ms":0.5}""", forecast))
        assertEquals("no place called \"Atlantis\" was found", result.getString("error"))
    }

    @Test
    fun noNetworkIsAnError() {
        val result = weather("""{"location":"Tokyo"}""", web(null, null))
        assertEquals("the weather service couldn't be reached", result.getString("error"))
    }

    @Test
    fun aPlaceIsNeeded() {
        val result = weather("""{}""", web(tokyo, forecast))
        assertTrue(result.has("error"))
        assertTrue("nothing requested", requested.isEmpty())
    }

    @Test
    fun weatherCodesAreDescribed() {
        assertEquals("clear sky", WeatherTools.describe(0))
        assertEquals("thunderstorm with hail", WeatherTools.describe(99))
        assertEquals("unknown", WeatherTools.describe(42))
    }
}
