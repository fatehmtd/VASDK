// CurlHttpProvider::cancel(): a request stuck waiting for a response is aborted promptly with
// NetworkErrorCode::Cancelled (without firing onError), and requests started afterwards work.
//
// A local server accepts connections: the first one never gets a reply, the second gets "ok".

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
    serverThread.join();
    close(server);

    std::printf("cancelled after %.2fs (code=%d), onError=%d, next request: status=%d body='%s'\n",
                cancelSeconds, static_cast<int>(stalledResp.error.code), errorReported.load(),
                okResp.statusCode, okResp.body.c_str());

    const bool ok = stalledResp.error.code == NetworkErrorCode::Cancelled && cancelSeconds < 0.8 &&
                    !errorReported && okResp.error.ok() && okResp.body == "ok";
    return ok ? 0 : 1;
}
