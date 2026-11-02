// CurlHttpProvider::cancel(): a request stuck waiting for a response, or cancelled in the middle of a
// streamed reply, is aborted promptly with NetworkErrorCode::Cancelled (without firing onError), and
// requests started afterwards work. HttpTransport stays usable after a failed request.
//
// A local server accepts connections: the first never gets a reply, the second gets "ok", the third
// streams one SSE chunk and then stalls.

#include <VASDK/HttpTransport.h>
#include <VASDK/network/providers/CurlHttpProvider.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace va::network;

namespace {
// Exposes the protected request helper.
struct ProbeTransport : va::transport::HttpTransport {
    using HttpTransport::httpPost;
};
} // namespace

int main() {
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    socklen_t len = sizeof(addr);
    if (server < 0 || bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(server, 4) != 0 || getsockname(server, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        std::fprintf(stderr, "could not start local server\n");
        return 1;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + "/";

    std::thread serverThread([server] {
        const int stalled = accept(server, nullptr, nullptr);  // never answered
        const int ok      = accept(server, nullptr, nullptr);
        char buf[4096];
        (void)!read(ok, buf, sizeof(buf));
        const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        (void)!write(ok, reply, sizeof(reply) - 1);
        close(ok);
        const int stream = accept(server, nullptr, nullptr);
        (void)!read(stream, buf, sizeof(buf));
        const char head[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\ndata: {\"a\":1}\n\n";
        (void)!write(stream, head, sizeof(head) - 1);
        std::this_thread::sleep_for(std::chrono::seconds(3));  // the rest of the reply never comes in time
        close(stream);
        close(stalled);
    });

    providers::CurlHttpProvider http;
    std::atomic<bool> errorReported{false};
    http.onError = [&](const NetworkError&) { errorReported = true; };

    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        http.cancel();
    });
    const auto t0 = std::chrono::steady_clock::now();
    const auto stalledResp = http.post(url, {}, "{}", 10000);
    const double cancelSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    canceller.join();

    const auto okResp = http.post(url, {}, "{}", 5000);

    // Mid-stream cancel, like a barge-in while the LLM is streaming.
    const auto t1 = std::chrono::steady_clock::now();
    const auto streamResp = http.post(url, {}, "{}", 10000, [&](const std::string&) { http.cancel(); });
    const double streamSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    serverThread.join();
    close(server);

    // A failed request (nothing listening) is reported but leaves HttpTransport connected.
    ProbeTransport transport;
    std::atomic<int> transportErrors{0};
    transport.onError = [&](int, const std::string&) { ++transportErrors; };
    transport.start();
    const auto refused = transport.httpPost(url, {}, "{}", 2000);
    const bool transportOk = !refused.error.ok() && transportErrors == 1 && transport.isConnected();
    std::printf("mid-stream cancel after %.2fs (code=%d); transport after failed request: connected=%d errors=%d\n",
                streamSeconds, static_cast<int>(streamResp.error.code), transport.isConnected(), transportErrors.load());

    std::printf("cancelled after %.2fs (code=%d), onError=%d, next request: status=%d body='%s'\n",
                cancelSeconds, static_cast<int>(stalledResp.error.code), errorReported.load(),
                okResp.statusCode, okResp.body.c_str());

    const bool ok = stalledResp.error.code == NetworkErrorCode::Cancelled && cancelSeconds < 0.8 &&
                    !errorReported && okResp.error.ok() && okResp.body == "ok" &&
                    streamResp.error.code == NetworkErrorCode::Cancelled && streamSeconds < 0.8 && transportOk;
    return ok ? 0 : 1;
}
