package com.vasdk.android.tools

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

object WebTools {

    val toolNames = setOf("web_search", "fetch_url")

    fun owns(toolName: String) = toolName in toolNames

    fun handle(context: Context, toolName: String, argsJson: String): String = when (toolName) {
        "web_search" -> webSearch(argsJson)
        "fetch_url"  -> fetchUrl(argsJson)
        else         -> JSONObject().put("error", "unknown tool: $toolName").toString()
    }

    private const val CONNECT_TIMEOUT_MS = 8_000
    private const val READ_TIMEOUT_MS    = 10_000
    private const val PREVIEW_CHARS      = 4 * 1024     // fetch_url: the start of a page
    private const val MAX_JSON_CHARS     = 256 * 1024   // an API's JSON, read whole (a cut one doesn't parse)

    //  web_search 
    // Uses DuckDuckGo Instant Answer API (JSON, no key needed).

    private fun webSearch(argsJson: String): String {
        val query = JSONObject(argsJson).getString("query")
        val encoded = java.net.URLEncoder.encode(query, "UTF-8")
        val urlStr  = "https://api.duckduckgo.com/?q=$encoded&format=json&no_html=1&skip_disambig=1"

        return try {
            val raw = httpGet(urlStr, MAX_JSON_CHARS) ?: return JSONObject().put("error", "Network request failed").toString()
            val ddg = JSONObject(raw)

            val results = JSONArray()

            // Abstract (single best answer)
            val abstract = ddg.optString("Abstract", "")
            val abstractUrl = ddg.optString("AbstractURL", "")
            if (abstract.isNotEmpty()) {
                results.put(JSONObject().apply {
                    put("title",   ddg.optString("Heading", query))
                    put("snippet", abstract)
                    put("url",     abstractUrl)
                })
            }

            // Related topics
            val topics = ddg.optJSONArray("RelatedTopics") ?: JSONArray()
            var i = 0
            while (i < topics.length() && results.length() < 5) {
                val topic = topics.optJSONObject(i++) ?: continue
                val text  = topic.optString("Text", "")
                val url   = topic.optJSONObject("Icon")?.let { topic.optString("FirstURL", "") }
                            ?: topic.optString("FirstURL", "")
                if (text.isNotEmpty()) {
                    results.put(JSONObject().apply {
                        put("title",   text.substringBefore(" - "))
                        put("snippet", text)
                        put("url",     url)
                    })
                }
            }

            JSONObject().apply {
                put("query",   query)
                put("results", results)
            }.toString()
        } catch (e: Exception) {
            JSONObject().put("error", e.message ?: "search failed").toString()
        }
    }

    //  fetch_url 

    private fun fetchUrl(argsJson: String): String {
        val url = JSONObject(argsJson).getString("url")
        if (!url.startsWith("https://") && !url.startsWith("http://"))
            return JSONObject().put("error", "URL must start with https:// or http://").toString()

        return try {
            val body = httpGet(url, PREVIEW_CHARS, truncate = true) ?: return JSONObject().put("error", "Network request failed").toString()
            JSONObject().apply {
                put("url",         url)
                put("length",      body.length)
                put("content",     body)
            }.toString()
        } catch (e: Exception) {
            JSONObject().put("error", e.message ?: "fetch failed").toString()
        }
    }

    //  HTTP helper 

    /** The body of [urlStr], or null when the request fails or the body is over [maxChars] (with
     *  [truncate], its first [maxChars] instead, marked as cut). */
    fun httpGet(urlStr: String, maxChars: Int = MAX_JSON_CHARS, truncate: Boolean = false): String? {
        val conn = URL(urlStr).openConnection() as HttpURLConnection
        return try {
            conn.connectTimeout = CONNECT_TIMEOUT_MS
            conn.readTimeout    = READ_TIMEOUT_MS
            conn.requestMethod  = "GET"
            conn.setRequestProperty("User-Agent", "VASDK/1.0")
            conn.connect()
            if (conn.responseCode !in 200..299) return null
            conn.inputStream.bufferedReader().use { reader ->
                val sb = StringBuilder()
                val buf = CharArray(1024)
                var read: Int
                while (reader.read(buf).also { read = it } != -1) {
                    sb.append(buf, 0, read)
                    if (sb.length >= maxChars) {
                        if (!truncate) return null   // a cut JSON wouldn't parse
                        sb.setLength(maxChars)
                        sb.append("\n[...truncated]")
                        break
                    }
                }
                sb.toString()
            }
        } finally {
            conn.disconnect()
        }
    }
}
