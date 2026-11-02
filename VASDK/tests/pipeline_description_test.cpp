// Pipeline::describeAsJson lists processors in chain order with their providers, and providers
// default to their class name without namespaces (shortTypeName), so UIs can show what is running.

#include <VASDK/Pipeline.h>
#include <VASDK/VADFrameProcessor.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace va;

namespace test_namespace {
class FakeVoiceActivityDetector : public audio::VADProvider {
public:
    bool initialize(const audio::AudioConfig&, int) override { return true; }
    void release() override {}
    std::vector<VADResult> detectVoiceActivity(const char*, size_t, size_t) const override { return {}; }
};

class QuotedProcessor : public pipeline::FrameProcessor {
public:
    std::string getName() const override { return "Quoted"; }
    std::string getProviderName() const override { return "model \"x\\y\""; }
    void processFrame(std::shared_ptr<transport::Frame>, pipeline::FlowDirection) override {}
};
} // namespace test_namespace

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
    };

    test_namespace::FakeVoiceActivityDetector detector;
    check(detector.getName() == "FakeVoiceActivityDetector", "provider name drops namespaces");
    check(shortTypeName(typeid(std::vector<int>)) == "vector", "template arguments are dropped");

    pipeline::Pipeline pipeline;
    pipeline.addProcessor(std::make_shared<pipeline::VADFrameProcessor>(
                              std::make_shared<test_namespace::FakeVoiceActivityDetector>()))
        .addProcessor(std::make_shared<test_namespace::QuotedProcessor>());
    const std::string json = pipeline.describeAsJson();
    const std::string expected = R"({"processors":[{"name":"VADFrameProcessor","provider":"FakeVoiceActivityDetector"},)"
                                 R"({"name":"Quoted","provider":"model \"x\\y\""}]})";
    check(json == expected, "chain order, providers and escaping");
    if (json != expected) std::fprintf(stderr, "  got %s\n", json.c_str());

    if (failures == 0) std::printf("pipeline_description: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
