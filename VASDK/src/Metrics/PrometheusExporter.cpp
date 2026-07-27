// PrometheusExporter.cpp - compiled only when VASDK_METRICS_PROMETHEUS=ON
//
// Exposes a Prometheus text exposition format endpoint at /metrics.
// Uses cpp-httplib (header-only, fetched via FetchContent when the option is ON).

#include <VASDK/metrics/exporters/PrometheusExporter.h>

// Pull in the full httplib header here (not in the public header).
#include <httplib.h>

#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <chrono>

namespace va {
namespace metrics {

PrometheusExporter::PrometheusExporter(MetricsCollector& collector,
                                       const PrometheusConfig& config)
    : _collector(collector)
    , _config(config)
    , _server(std::make_unique<httplib::Server>())
{}

PrometheusExporter::~PrometheusExporter()
{
    stop();
}

void PrometheusExporter::setConfig(const PrometheusConfig& config)
{
    std::lock_guard<std::mutex> lk(_configMutex);
    _config = config;
}

void PrometheusExporter::start()
{
    if (_running.exchange(true)) return;

    // Register GET /metrics handler - httplib is thread-safe for concurrent requests
    _server->Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(buildPage(), "text/plain; version=0.0.4; charset=utf-8");
    });

    _serverThread = std::thread(&PrometheusExporter::serveLoop, this);
    spdlog::info("PrometheusExporter listening on {}:{}",
                 _config.bindAddress, _config.port);
}

void PrometheusExporter::stop()
{
    if (!_running.exchange(false)) return;
    _server->stop();
    if (_serverThread.joinable()) _serverThread.join();
}

//  Private 

void PrometheusExporter::serveLoop()
{
    std::string addr;
    int         port;
    {
        std::lock_guard<std::mutex> lk(_configMutex);
        addr = _config.bindAddress;
        port = _config.port;
    }
    // listen() blocks until stop() is called
    if (!_server->listen(addr.c_str(), port)) {
        spdlog::error("PrometheusExporter: failed to bind on {}:{}", addr, port);
        _running = false;
    }
}

std::string PrometheusExporter::buildPage()
{
    bool includeHelp, includeType;
    {
        std::lock_guard<std::mutex> lk(_configMutex);
        includeHelp = _config.includeHelp;
        includeType = _config.includeType;
    }

    auto snap = _collector.snapshot();
    std::ostringstream o;

    //  Counters 
    for (const auto& c : snap.counters) {
        if (includeHelp) o << "# HELP " << c.name << " VASDK counter\n";
        if (includeType) o << "# TYPE " << c.name << " counter\n";
        o << c.name << " " << c.value << "\n";
    }

    //  Gauges 
    for (const auto& g : snap.gauges) {
        if (includeHelp) o << "# HELP " << g.name << " VASDK gauge\n";
        if (includeType) o << "# TYPE " << g.name << " gauge\n";
        o << g.name << " " << g.value << "\n";
    }

    //  Samplers (exposed as summary-style gauge metrics) 
    for (const auto& s : snap.samplers) {
        if (s.totalObserved == 0) continue;
        const std::string& base = s.name;
        if (includeHelp) o << "# HELP " << base << " VASDK sliding-window sampler\n";
        if (includeType) o << "# TYPE " << base << " summary\n";
        // Quantiles
        for (size_t i = 0; i < s.quantiles.size() && i < s.quantileValues.size(); ++i) {
            o << base << "{quantile=\"" << s.quantiles[i] << "\"} "
              << s.quantileValues[i] << "\n";
        }
        o << base << "_sum "   << (s.mean * static_cast<double>(s.totalObserved)) << "\n";
        o << base << "_count " << s.totalObserved << "\n";
        // Also expose mean as a gauge for convenience
        o << base << "_mean " << s.mean << "\n";
    }

    return o.str();
}

} // namespace metrics
} // namespace va
