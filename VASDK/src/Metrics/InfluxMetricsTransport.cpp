// InfluxMetricsTransport.cpp - compiled only when VASDK_METRICS_INFLUX=ON

#include <VASDK/metrics/exporters/InfluxMetricsTransport.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <condition_variable>
#include <sstream>
#include <thread>

#ifdef VASDK_HAS_CURL
#  include <curl/curl.h>
#endif

namespace va {
namespace metrics {

InfluxMetricsTransport::InfluxMetricsTransport(MetricsCollector& collector,
                                               const InfluxConfig& config)
    : _collector(collector)
    , _config(config)
{}

InfluxMetricsTransport::~InfluxMetricsTransport()
{
    stop();
}

void InfluxMetricsTransport::setConfig(const InfluxConfig& config)
{
    std::lock_guard<std::mutex> lk(_configMutex);
    _config = config;
}

void InfluxMetricsTransport::start()
{
    if (_running.exchange(true)) return;
    _flushThread = std::thread(&InfluxMetricsTransport::flushLoop, this);
    spdlog::debug("InfluxMetricsTransport started → {}/{}", _config.url, _config.bucket);
}

void InfluxMetricsTransport::stop()
{
    if (!_running.exchange(false)) return;
    _cv.notify_one();
    if (_flushThread.joinable()) _flushThread.join();
    flush(); // drain any remaining lines
}

void InfluxMetricsTransport::onSnapshot(const MetricsSnapshot& snapshot)
{
    // Convert snapshot to Line Protocol and queue lines
    for (const auto& c : snapshot.counters) {
        appendLine(c.name, "value=" + std::to_string(c.value) + "u", snapshot.capturedAtNs);
    }
    for (const auto& g : snapshot.gauges) {
        appendLine(g.name, "value=" + std::to_string(g.value), snapshot.capturedAtNs);
    }
    for (const auto& s : snapshot.samplers) {
        if (s.totalObserved == 0) continue;
        std::string fields = "mean=" + std::to_string(s.mean) +
                             ",total=" + std::to_string(s.totalObserved) + "u";
        for (size_t i = 0; i < s.quantiles.size() && i < s.quantileValues.size(); ++i) {
            std::string p = "p" + std::to_string(static_cast<int>(s.quantiles[i] * 100));
            fields += "," + p + "=" + std::to_string(s.quantileValues[i]);
        }
        appendLine(s.name, fields, snapshot.capturedAtNs);
    }

    // Flush opportunistically if batch threshold reached
    bool shouldFlush;
    {
        std::lock_guard<std::mutex> lk(_linesMutex);
        shouldFlush = static_cast<int>(_pendingLines.size()) >= _config.batchSize;
    }
    if (shouldFlush) flush();
}

void InfluxMetricsTransport::onUtteranceSummary(const MetricsSnapshot& utteranceSnapshot)
{
    onSnapshot(utteranceSnapshot);
}

//  Private 

void InfluxMetricsTransport::flushLoop()
{
    int intervalMs;
    {
        std::lock_guard<std::mutex> lk(_configMutex);
        intervalMs = _config.flushIntervalMs;
    }
    while (_running) {
        {
            std::unique_lock<std::mutex> lk(_cvMutex);
            _cv.wait_for(lk, std::chrono::milliseconds(intervalMs),
                         [this] { return !_running.load(); });
        }
        if (_running) flush();
    }
}

void InfluxMetricsTransport::flush()
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(_linesMutex);
        if (_pendingLines.empty()) return;
        lines.swap(_pendingLines);
    }

    std::string body;
    body.reserve(lines.size() * 80);
    for (const auto& line : lines) {
        body += line;
        body += '\n';
    }
    if (!httpPost(body)) {
        spdlog::warn("InfluxMetricsTransport: HTTP POST failed, {} lines dropped", lines.size());
    }
}

void InfluxMetricsTransport::appendLine(const std::string& measurement,
                                        const std::string& fields,
                                        uint64_t timestampNs)
{
    // Line Protocol: <measurement>[,tags] <fields> <timestamp>
    std::string line = measurement + " " + fields + " " + std::to_string(timestampNs);
    std::lock_guard<std::mutex> lk(_linesMutex);
    _pendingLines.push_back(std::move(line));
}

bool InfluxMetricsTransport::httpPost(const std::string& body)
{
#ifdef VASDK_HAS_CURL
    std::string url, org, bucket, token;
    {
        std::lock_guard<std::mutex> lk(_configMutex);
        url    = _config.url + "/api/v2/write?org=" + _config.org +
                 "&bucket=" + _config.bucket + "&precision=ns";
        token  = _config.token;
    }

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: text/plain; charset=utf-8");
    if (!token.empty()) {
        std::string authHeader = "Authorization: Token " + token;
        headers = curl_slist_append(headers, authHeader.c_str());
    }

    long responseCode = 0;
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        5L);
    // Silence output
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char*, size_t size, size_t nmemb, void*) { return size * nmemb; });

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK && responseCode >= 200 && responseCode < 300);
#else
    (void)body;
    spdlog::warn("InfluxMetricsTransport: libcurl not available, HTTP POST skipped");
    return false;
#endif
}

} // namespace metrics
} // namespace va
