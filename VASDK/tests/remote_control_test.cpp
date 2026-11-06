// Remote control over HTTP: the page; no token or a wrong one gets 401; a command in CONTROL
// mode drives the session and its reply arrives on the event stream; OBSERVE refuses commands
// (403) but streams events; without a token, it doesn't start, nor does the session. With tls, it
// serves HTTPS (a self-signed certificate the test makes), and refuses files that aren't PEM.

#include "engine_test_fakes.h"

#include <VASDK/VAEngine.h>
#include <VASDK/engine/ProtoFileLoader.h>
#include <VASDK/engine/RemoteControl.h>
#include <VASDK/flow/CompiledAgent.h>

#include <httplib.h>

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

using namespace va;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}

const char* kAgent = R"(
  start_flow: "main"
  intents { name: "hours" examples: ["hours"] }
  flows { name: "main" start_page: "welcome"
    pages { name: "welcome" on_enter { ask { say { text: "How can I help?" } } }
            routes { intent: "hours" steps { say { text: "We're open 8 to 6." } } } } }
)";

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
// A self-signed certificate for 127.0.0.1 and its key, as PEM files in `folder`.
bool makeCertificate(const std::filesystem::path& folder, std::string& cert, std::string& key) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    X509* x509 = X509_new();
    if (!pkey || !x509) return false;
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 60L * 60 * 24);
    X509_set_pubkey(x509, pkey);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    const bool signedOk = X509_sign(x509, pkey, EVP_sha256()) > 0;
    cert = (folder / "remote_control_test.crt").string();
    key = (folder / "remote_control_test.key").string();
    // Through OpenSSL's own files: a FILE* from this binary's C runtime can't cross into its DLL on Windows.
    BIO* out = BIO_new_file(cert.c_str(), "wb");
    const bool certOk = out && PEM_write_bio_X509(out, x509);
    BIO_free(out);
    out = BIO_new_file(key.c_str(), "wb");
    const bool keyOk = out && PEM_write_bio_PrivateKey(out, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    BIO_free(out);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return signedOk && certOk && keyOk;
}
#endif

// Reads the event stream until `wanted` is in it, or the time is up.
bool streamShows(uint16_t port, const std::string& query, const std::string& wanted, const std::function<void()>& meanwhile) {
    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(5, 0);
    std::string seen;
    std::atomic<bool> found{false};
    std::thread reader([&] {
        client.Get("/events" + query, [&](const char* data, size_t length) {
            seen.append(data, length);
            if (seen.find(wanted) != std::string::npos) found = true;
            return !found.load();
        });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    meanwhile();
    for (int i = 0; i < 100 && !found; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.stop();
    reader.join();
    return found;
}

} // namespace

int main() {
    std::string error;
    vasdk::flow::Agent definition;
    engine::parseProto(kAgent, engine::ProtoFormat::Text, definition, &error);
    auto agent = flow::CompiledAgent::compile(definition, &error);
    check(agent != nullptr, "the agent compiles: " + error);
    if (!agent) return 1;

    auto tts = std::make_shared<fakes::FakeTts>();
    VAEngine::Options options;
    options.transport = std::make_shared<fakes::FakeTransport>();
    options.agent = agent;
    auto session = VAEngine::create(vasdk::engine::EngineConfig{}, fakes::makeRegistry(tts), options, &error);
    if (!session || !session->start(&error)) { std::fprintf(stderr, "  %s\n", error.c_str()); return 1; }

    vasdk::engine::RemoteControl config;
    config.set_enabled(true);
    config.set_bind("127.0.0.1:0");
    check(!engine::RemoteControl::start(*session, config, &error) && error.find("token") != std::string::npos,
          "no token: it doesn't start");
    config.set_token("s3cret-token");
    config.set_mode(vasdk::engine::RemoteControl::CONTROL);
    auto remote = engine::RemoteControl::start(*session, config, &error);
    check(remote && remote->port() > 0, "it listens on a free port: " + error);
    if (!remote) return 1;
    httplib::Client client("127.0.0.1", remote->port());
    const httplib::Headers bearer{{"Authorization", "Bearer s3cret-token"}};

    auto page = client.Get("/");
    check(page && page->status == 200 && page->body.find("EventSource") != std::string::npos, "the trace page");
    auto noToken = client.Get("/events");
    check(noToken && noToken->status == 401, "events without the token: 401");
    auto wrong = client.Post("/command", httplib::Headers{{"Authorization", "Bearer guess"}}, R"({"injectText": "hours"})",
                             "application/json");
    check(wrong && wrong->status == 401, "a wrong token: 401");
    auto bad = client.Post("/command", bearer, R"({"nope": 1})", "application/json");
    check(bad && bad->status == 400, "not a command: 400");

    // A command drives the session; its reply arrives on the stream (the token in the address, as EventSource sends it).
    check(streamShows(remote->port(), "?token=s3cret-token", "We're open 8 to 6.", [&] {
              auto sent = client.Post("/command", bearer, R"({"injectText": "hours"})", "application/json");
              check(sent && sent->status == 200, "the command is taken");
          }),
          "its reply arrives on the event stream");

    // Observe only: events, no commands.
    remote.reset();
    config.set_mode(vasdk::engine::RemoteControl::OBSERVE);
    auto observer = engine::RemoteControl::start(*session, config, &error);
    check(observer != nullptr, "an observer starts: " + error);
    if (observer) {
        httplib::Client watcher("127.0.0.1", observer->port());
        auto refused = watcher.Post("/command", bearer, R"({"injectText": "hours"})", "application/json");
        check(refused && refused->status == 403, "observe only: commands get 403");
        check(streamShows(observer->port(), "?token=s3cret-token", "We're open 8 to 6.", [&] { session->injectUserText("hours"); }),
              "but events stream");
    }
    observer.reset();

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    // HTTPS: the same server, encrypted.
    {
        const auto folder = std::filesystem::temp_directory_path() / "vasdk_remote_control_test";
        std::filesystem::create_directories(folder);
        std::string cert, key;
        check(makeCertificate(folder, cert, key), "a self-signed certificate");
        config.mutable_tls()->set_cert_file(cert);
        config.mutable_tls()->set_key_file(key);
        auto secure = engine::RemoteControl::start(*session, config, &error);
        check(secure != nullptr, "it serves HTTPS: " + error);
        if (secure) {
            httplib::SSLClient https("127.0.0.1", secure->port());
            https.enable_server_certificate_verification(false);   // self-signed
            auto page = https.Get("/");
            check(page && page->status == 200 && page->body.find("EventSource") != std::string::npos, "the page over HTTPS");
            auto noToken = https.Get("/events");
            check(noToken && noToken->status == 401, "and the token is still needed");
            httplib::Client plain("127.0.0.1", secure->port());
            plain.set_connection_timeout(2, 0);
            plain.set_read_timeout(2, 0);
            auto clear = plain.Get("/");
            check(!clear || clear->status != 200, "plain HTTP isn't served");
        }
        secure.reset();
        config.mutable_tls()->set_cert_file((folder / "missing.crt").string());
        check(!engine::RemoteControl::start(*session, config, &error) && error.find("tls") != std::string::npos,
              "a certificate that isn't there: it doesn't start");
        config.clear_tls();
        std::filesystem::remove_all(folder);
    }
#endif
    session->stop();

    // A session whose config enables it without a token doesn't start.
    vasdk::engine::EngineConfig withRemote;
    withRemote.mutable_remote_control()->set_enabled(true);
    VAEngine::Options second;
    second.transport = std::make_shared<fakes::FakeTransport>();
    second.agent = agent;
    auto unsafe = VAEngine::create(withRemote, fakes::makeRegistry(std::make_shared<fakes::FakeTts>()), second, &error);
    check(unsafe && !unsafe->start(&error) && error.find("token") != std::string::npos, "a session with remote_control and no token fails to start");

    if (failures == 0) std::printf("remote_control_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
