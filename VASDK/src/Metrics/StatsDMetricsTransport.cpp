// StatsDMetricsTransport.cpp - compiled only when VASDK_METRICS_STATSD=ON

#include <VASDK/metrics/exporters/StatsDMetricsTransport.h>
#include <spdlog/spdlog.h>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using SocketType = SOCKET;
   static constexpr SocketType kInvalidSocket = INVALID_SOCKET;
   static inline void closeSocket(SocketType s) { closesocket(s); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   using SocketType = int;
   static constexpr SocketType kInvalidSocket = -1;
   static inline void closeSocket(SocketType s) { ::close(s); }
#endif

namespace va {
namespace metrics {

StatsDMetricsTransport::StatsDMetricsTransport(MetricsCollector& collector,
                                               const StatsDConfig& config)
    : _collector(collector)
    , _config(config)
{}

StatsDMetricsTransport::~StatsDMetricsTransport()
{
    stop();
}

void StatsDMetricsTransport::setConfig(const StatsDConfig& config)
{
    std::lock_guard<std::mutex> lk(_configMutex);
    _config = config;
}

void StatsDMetricsTransport::start()
{
    if (_running.exchange(true)) return;

#ifdef _WIN32
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
#endif

    _sockFd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (_sockFd == static_cast<int>(kInvalidSocket)) {
        spdlog::error("StatsDMetricsTransport: failed to create UDP socket");
        _running = false;
        return;
    }

    _flushThread = std::thread(&StatsDMetricsTransport::flushLoop, this);
    spdlog::debug("StatsDMetricsTransport started → {}:{}", _config.host, _config.port);
}

void StatsDMetricsTransport::stop()
{
    if (!_running.exchange(false)) return;
    _cv.notify_one();
    if (_flushThread.joinable()) _flushThread.join();
    if (_sockFd != static_cast<int>(kInvalidSocket)) {
        closeSocket(static_cast<SocketType>(_sockFd));
        _sockFd = static_cast<int>(kInvalidSocket);
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

void StatsDMetricsTransport::onSnapshot(const MetricsSnapshot& snapshot)
{
    // Push-based: called by MetricsReporter - forward to send immediately
    sendSnapshot(snapshot);
}

//  Private 

void StatsDMetricsTransport::flushLoop()
{
    int intervalMs;
    {
        std::lock_guard<std::mutex> lk(_configMutex);
        intervalMs = _config.flushIntervalMs;
    }
    while (_running) {
        auto snap = _collector.snapshot();
        sendSnapshot(snap);
        std::unique_lock<std::mutex> lk(_cvMutex);
        _cv.wait_for(lk, std::chrono::milliseconds(intervalMs),
                     [this] { return !_running.load(); });
    }
}

void StatsDMetricsTransport::sendSnapshot(const MetricsSnapshot& snapshot)
{
    std::string prefix;
    {
        std::lock_guard<std::mutex> lk(_configMutex);
        prefix = _config.prefix;
    }

    // Build a batch payload delimited by '\n' (StatsD protocol allows multi-metric datagrams)
    std::string payload;
    payload.reserve(1024);

    // Counters - send delta since last snapshot
    {
        std::lock_guard<std::mutex> lk(_cacheMutex);
        for (const auto& c : snapshot.counters) {
            uint64_t last = 0;
            auto it = std::find_if(_counterCache.begin(), _counterCache.end(),
                [&c](const auto& p){ return p.first == c.name; });
            if (it == _counterCache.end()) {
                _counterCache.emplace_back(c.name, CounterCache{});
                it = _counterCache.end() - 1;
            }
            last = it->second.last;
            uint64_t delta = (c.value >= last) ? (c.value - last) : c.value;
            it->second.last = c.value;
            if (delta > 0) {
                payload += prefix + c.name + ":" + std::to_string(delta) + "|c\n";
            }
        }
    }

    // Gauges - send current value
    for (const auto& g : snapshot.gauges) {
        payload += prefix + g.name + ":" + std::to_string(g.value) + "|g\n";
    }

    // Samplers - send mean and p50/p95/p99 as timing metrics
    for (const auto& s : snapshot.samplers) {
        if (s.totalObserved == 0) continue;
        payload += prefix + s.name + ".mean:" + std::to_string(s.mean) + "|ms\n";
        for (size_t i = 0; i < s.quantiles.size() && i < s.quantileValues.size(); ++i) {
            std::string pLabel = "p" + std::to_string(static_cast<int>(s.quantiles[i] * 100));
            payload += prefix + s.name + "." + pLabel + ":" +
                       std::to_string(s.quantileValues[i]) + "|ms\n";
        }
    }

    if (!payload.empty()) {
        sendUDP(payload);
    }
}

void StatsDMetricsTransport::sendUDP(const std::string& payload)
{
    if (_sockFd == static_cast<int>(kInvalidSocket)) return;

    std::string host;
    uint16_t    port;
    {
        std::lock_guard<std::mutex> lk(_configMutex);
        host = _config.host;
        port = _config.port;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        spdlog::warn("StatsDMetricsTransport: invalid host '{}'", host);
        return;
    }

    ::sendto(_sockFd,
             payload.c_str(),
             static_cast<int>(payload.size()),
             0,
             reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr));
}

} // namespace metrics
} // namespace va
