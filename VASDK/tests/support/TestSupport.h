// What every VASDK test shares (skills/vasdk-testing, docs/testing.md): waiting for something to
// happen, a temporary folder of its own, and the models folder a model-gated test reads.
#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

namespace va::test {

/// Until `condition` holds, at most `timeout`; whether it held. A test checks that something
/// happens by waiting for it, never by sleeping a fixed time: a loaded machine (a shared CI
/// runner) runs late, never early. A fixed wait is only for checking that something does *not*
/// happen, and says so with a `// fixed wait:` comment.
template <class Condition>
bool waitUntil(Condition condition, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

/// A new, empty folder under the system's temporary folder, removed with everything in it when
/// this goes. Its name is random, so tests running side by side (ctest -j) never share one.
class UniqueTemporaryDirectory {
public:
    explicit UniqueTemporaryDirectory(const std::string& prefix = "vasdk_test") {
        std::random_device random;
        std::ostringstream name;
        name << prefix << '_' << std::hex << random() << random();
        _path = std::filesystem::temp_directory_path() / name.str();
        std::filesystem::create_directories(_path);
    }
    ~UniqueTemporaryDirectory() {
        std::error_code ignored;   // a file still open (Windows) leaves the folder behind, never a failure
        std::filesystem::remove_all(_path, ignored);
    }
    UniqueTemporaryDirectory(const UniqueTemporaryDirectory&) = delete;
    UniqueTemporaryDirectory& operator=(const UniqueTemporaryDirectory&) = delete;

    const std::filesystem::path& path() const { return _path; }
    std::filesystem::path operator/(const std::string& name) const { return _path / name; }

private:
    std::filesystem::path _path;
};

/// The folder vasdk_add_gtest(... MODELS_DIR) gave this program (--vasdk-models-dir), or the
/// VASDK_TEST_MODELS_DIR environment variable when set, which wins; empty when neither is.
inline std::filesystem::path modelsDirectory() {
    const char* folder = std::getenv("VASDK_TEST_MODELS_DIR");
    return folder ? std::filesystem::path(folder) : std::filesystem::path();
}

} // namespace va::test

/// Skips the test (GTEST_SKIP) when `requiredFile` doesn't exist: a model or data file not
/// downloaded. The skip names the missing file. In a test body or a fixture's SetUp.
#define VA_REQUIRE_FILE(requiredFile)                                                           \
    do {                                                                                        \
        const std::filesystem::path vasdkRequiredFile_(requiredFile);                           \
        if (!std::filesystem::exists(vasdkRequiredFile_))                                       \
            GTEST_SKIP() << "missing " << vasdkRequiredFile_.string();                          \
    } while (false)
