// Engine config loading: text proto, JSON and binary give the same message; unknown fields
// fail with a line:column; ${ENV} expansion (with fallback, and an error when unset); Any
// options in the expanded text syntax.

#include <VASDK/engine/ProtoFileLoader.h>
#include <vasdk/engine/engine_config.pb.h>

#include <google/protobuf/util/message_differencer.h>
#include <google/protobuf/wrappers.pb.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace va::engine;
namespace fs = std::filesystem;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what); }
}

void setEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");   // "" unsets on Windows
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

fs::path writeFile(const std::string& name, const std::string& content) {
    auto path = fs::temp_directory_path() / name;
    std::ofstream(path, std::ios::binary) << content;
    return path;
}

const char* kText = R"(
# proto-message: vasdk.engine.EngineConfig
models_dir: "models"
providers {
  stt { id: "parakeet" fallback: ["moonshine"] }
  tts {
    id: "pocket"
    options { [type.googleapis.com/google.protobuf.StringValue] { value: "jean" } }
  }
}
llm { endpoint: "${VASDK_TEST_ENDPOINT}" model: "${VASDK_TEST_UNSET:-qwen}" }
)";

void testFormatsAgree() {
    setEnvironment("VASDK_TEST_ENDPOINT", "http://localhost:8080");
    setEnvironment("VASDK_TEST_UNSET", nullptr);

    vasdk::engine::EngineConfig fromText;
    std::string error;
    check(loadEngineConfig(writeFile("vasdk_engine.txtpb", kText).string(), fromText, &error), "text loads");
    check(fromText.llm().endpoint() == "http://localhost:8080", "env: ${NAME} expanded");
    check(fromText.llm().model() == "qwen", "env: ${NAME:-fallback} used when unset");
    check(fromText.providers().stt().fallback_size() == 1, "repeated field parsed");

    google::protobuf::StringValue voice;
    check(fromText.providers().tts().options().UnpackTo(&voice) && voice.value() == "jean", "Any options unpack");

    vasdk::engine::EngineConfig fromJson;
    check(loadEngineConfig(writeFile("vasdk_engine.json", R"({
        "modelsDir": "models",
        "providers": {
          "stt": { "id": "parakeet", "fallback": ["moonshine"] },
          "tts": { "id": "pocket", "options": { "@type": "type.googleapis.com/google.protobuf.StringValue", "value": "jean" } } },
        "llm": { "endpoint": "${VASDK_TEST_ENDPOINT}", "model": "qwen" } })").string(), fromJson, &error),
          "json loads");
    check(google::protobuf::util::MessageDifferencer::Equals(fromText, fromJson), "json equals text");

    vasdk::engine::EngineConfig fromBinary;
    check(loadEngineConfig(writeFile("vasdk_engine.pb", fromText.SerializeAsString()).string(), fromBinary, &error),
          "binary loads");
    check(google::protobuf::util::MessageDifferencer::Equals(fromText, fromBinary), "binary equals text");
}

void testErrors() {
    vasdk::engine::EngineConfig config;
    std::string error;
    check(!parseProto("models_dir: \"m\"\nprovidrs { }\n", ProtoFormat::Text, config, &error), "unknown field fails");
    check(error.rfind("2:", 0) == 0 && error.find("providrs") != std::string::npos, "unknown field: line and name reported");

    check(!parseProto(R"({"modelsDir": 3})", ProtoFormat::Json, config, &error), "json type error fails");
    check(!parseProto(R"({"bogus": 1})", ProtoFormat::Json, config, &error), "json unknown field fails");

    setEnvironment("VASDK_TEST_UNSET", nullptr);
    auto path = writeFile("vasdk_engine_unset.txtpb", "models_dir: \"m\"\nllm { api_key: \"${VASDK_TEST_UNSET}\" }\n");
    check(!loadEngineConfig(path.string(), config, &error), "unset env var fails");
    check(error.find("line 2") != std::string::npos && error.find("VASDK_TEST_UNSET") != std::string::npos,
          "unset env var: line and name reported");

    // Comments are not expanded, even with an apostrophe or an unset variable in them.
    path = writeFile("vasdk_engine_comment.txtpb", "# don't use ${VASDK_TEST_UNSET}\nmodels_dir: \"m\"\n");
    check(loadEngineConfig(path.string(), config, &error), "comment with ${UNSET} ignored");

    // A value with quotes or backslashes cannot end the string it lands in.
    setEnvironment("VASDK_TEST_TRICKY", "a\"b\\c\" models_dir: \"evil");
    path = writeFile("vasdk_engine_tricky.txtpb", "llm { api_key: \"${VASDK_TEST_TRICKY}\" }\n");
    check(loadEngineConfig(path.string(), config, &error), "tricky value loads");
    check(config.llm().api_key() == "a\"b\\c\" models_dir: \"evil" && config.models_dir().empty(),
          "tricky value kept verbatim inside its string");
    std::string json;
    check(expandEnvironmentVariables(R"({"llm": {"apiKey": "${VASDK_TEST_TRICKY}"}})", json, &error)
          && parseProto(json, ProtoFormat::Json, config, &error)
          && config.llm().api_key() == "a\"b\\c\" models_dir: \"evil", "tricky value in JSON");

    // Without expansion (agent files), ${...} stays literal.
    path = writeFile("vasdk_engine_unset.txtpb", "models_dir: \"m\"\nllm { api_key: \"${VASDK_TEST_UNSET}\" }\n");
    check(loadProtoFile(path.string(), config, &error), "no expansion by default");
    check(config.llm().api_key() == "${VASDK_TEST_UNSET}", "no expansion keeps the text");

    check(!loadEngineConfig("engine.yaml", config, &error) && error.find("extension") != std::string::npos,
          "unknown extension rejected");
}

} // namespace

int main() {
    testFormatsAgree();
    testErrors();
    if (failures == 0) std::printf("proto_load_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
