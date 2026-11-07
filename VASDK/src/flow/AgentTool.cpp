#include "flow/AgentTool.h"
#include "engine/ProtoFileLoader.h"
#include "llm/Completion.h"
#include "engine/ProviderRegistry.h"
#include "nlu/EmbeddingNluProvider.h"
#include "nlu/LlmEntityExtractor.h"
#include "nlu/LlmNluProvider.h"
#include "nlu/RuleNluProvider.h"
#include "nlu/TaggerEntityExtractor.h"

#include <google/protobuf/util/json_util.h>
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <sstream>

#include "flow/CompiledAgent.h"
#include "flow/FlowRuntime.h"
#include "flow/FlowTestRunner.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <type_traits>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace va {
namespace flow {

namespace {

int usage(const std::string& program) {
    const char* name = program.c_str();
    std::fprintf(stderr,
                 "usage: %s lint <agent>\n"
                 "       %s test <agent> <case>...\n"
                 "       %s chat <agent> [--session name=value]...\n"
                 "         (an empty line is silence, #2 taps the second choice, !<trigger> {json} fires a trigger)\n"
                 "       %s try <agent> [--entity @name] <words>\n"
                 "       %s eval <agent> <set.txtpb> [--min-accuracy 0.8]   (intent accuracy, entity F1)\n"
                 "         [--sweep chat_min_share=0.15,0.25,0.5] [--sweep keyword_words=4,6,8] [--sweep min_confidence=...]\n"
                 "         (accuracy at each combination of the engine config's `understanding` values)\n"
                 "  with an LLM after the rules (test, chat, try, eval):\n"
                 "    --llm                   the endpoint, model and key from LLM_ENDPOINT, LLM_MODEL, LLM_API_KEY\n"
                 "                            (or OPENROUTER_API_KEY, OPENAI_API_KEY)\n"
                 "    --llm-endpoint <url>    an OpenAI-compatible chat completions URL; --llm-model, --llm-api-key\n"
                 "  with models (test, chat, try, eval):\n"
                 "    --engine-config <file>  its sentence_encoder, entity_model and understanding settings (minilm and\n"
                 "                            gliner need a tool built with the providers, such as garage_agentc;\n"
                 "                            entity_model llm needs --llm)\n",
                 name, name, name, name, name);
    return 2;
}

std::shared_ptr<const va::flow::CompiledAgent> load(const std::string& path, bool showWarnings,
                                                    std::shared_ptr<const FlowRegistry> registry) {
    std::string error;
    std::vector<std::string> warnings;
    auto agent = va::flow::CompiledAgent::load(path, &error, &warnings, std::move(registry));
    if (showWarnings)
        for (const auto& warning : warnings) std::fprintf(stderr, "%s: warning: %s\n", path.c_str(), warning.c_str());
    if (!agent) {
        std::string line;
        for (const char c : error + "\n") {
            if (c != '\n') { line += c; continue; }
            if (!line.empty()) std::fprintf(stderr, "%s: error: %s\n", path.c_str(), line.c_str());
            line.clear();
        }
    }
    return agent;
}

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

// Takes the --llm flags out of the arguments: an LLM to ask when given, else null.
std::shared_ptr<const llm::Completion> takeLlmFlags(std::vector<std::string>& arguments, std::string* error) {
    bool wanted = false;
    llm::LLMConfig config;
    for (size_t i = 0; i < arguments.size();) {
        const auto& flag = arguments[i];
        const bool valued = flag == "--llm-endpoint" || flag == "--llm-model" || flag == "--llm-api-key";
        if (flag != "--llm" && !valued) {
            ++i;
            continue;
        }
        if (valued && i + 1 >= arguments.size()) {
            *error = flag + " needs a value";
            return nullptr;
        }
        wanted = true;
        if (flag == "--llm-endpoint") config.endpoint = arguments[i + 1];
        if (flag == "--llm-model") config.model = arguments[i + 1];
        if (flag == "--llm-api-key") config.apiKey = arguments[i + 1];
        arguments.erase(arguments.begin() + static_cast<long>(i), arguments.begin() + static_cast<long>(i + (valued ? 2 : 1)));
    }
    if (!wanted) return nullptr;
    if (config.endpoint.empty()) config.endpoint = environment("LLM_ENDPOINT");
    if (config.model.empty()) config.model = environment("LLM_MODEL");
    for (const char* key : {"LLM_API_KEY", "OPENROUTER_API_KEY", "OPENAI_API_KEY"})
        if (config.apiKey.empty()) config.apiKey = environment(key);
    if (config.endpoint.empty()) {
        *error = "--llm: no endpoint (set LLM_ENDPOINT or pass --llm-endpoint)";
        return nullptr;
    }
    config.temperature = 0.0f;
    config.maxTokens = 400;
    config.timeoutMs = 20000;
    std::fprintf(stderr, "LLM: %s (%s)\n", config.endpoint.c_str(), config.model.empty() ? "default model" : config.model.c_str());
    return std::make_shared<llm::OpenAICompletion>(config);
}

// Takes --engine-config <file> out of the arguments: the config when given.
std::optional<vasdk::engine::EngineConfig> takeEngineConfig(std::vector<std::string>& arguments, std::string* error) {
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] != "--engine-config") continue;
        if (i + 1 >= arguments.size()) {
            *error = "--engine-config needs a file";
            return std::nullopt;
        }
        vasdk::engine::EngineConfig config;
        if (!engine::loadEngineConfig(arguments[i + 1], config, error)) return std::nullopt;
        arguments.erase(arguments.begin() + static_cast<long>(i), arguments.begin() + static_cast<long>(i + 2));
        return config;
    }
    return std::nullopt;
}

// The models folder beside the tool, as the apps look for it: VASDK_3P_IMPL/models up to 3 levels up.
std::string modelsNear(const std::string& program) {
    std::error_code error;
    auto directory = std::filesystem::canonical(program, error).parent_path();
    for (int level = 0; !error && level <= 3; ++level) {
        if (std::filesystem::exists(directory / "VASDK_3P_IMPL" / "models")) return (directory / "VASDK_3P_IMPL" / "models").string();
        if (directory == directory.parent_path()) break;
        directory = directory.parent_path();
    }
    return "models";
}

std::string describe(const va::flow::EntityMatch& match) {
    std::string text = "@" + match.entity + " = " + va::flow::toText(match.value);
    if (match.spoken != va::flow::toText(match.value)) text += "  spoken \"" + match.spoken + "\"";
    text += "  heard \"" + match.raw + "\"";
    for (const auto& [name, value] : match.attributes.fields()) text += "  " + name + "=" + va::flow::toText(value);
    return text;
}

void print(const va::flow::Reply& reply, const va::flow::FlowRuntime& conversation) {
    std::printf("bot: %s   [%s/%s]%s%s\n", reply.text.empty() ? "(nothing)" : reply.text.c_str(),
                conversation.flowName().c_str(), conversation.pageName().c_str(),
                reply.event.empty() ? "" : ("  (" + reply.event + ")").c_str(),
                reply.endsSession ? "  (conversation ended)" : "");
    const auto stage = conversation.lookup({"nlu", "stage"});
    if (stage.has_string_value() && stage.string_value() != "rules" && !reply.intent.empty())
        std::printf("     understood by: %s (%s)\n", stage.string_value().c_str(), reply.intent.c_str());
    for (const auto& segment : reply.segments) {
        if (!segment.interruptible) std::printf("     can't be interrupted: \"%s\"\n", segment.text.c_str());
        if (!segment.voice.empty() || segment.speed > 0)
            std::printf("     in voice %s at %.2gx: \"%s\"\n", segment.voice.empty() ? "(its own)" : segment.voice.c_str(),
                        segment.speed > 0 ? static_cast<double>(segment.speed) : 1.0, segment.text.c_str());
    }
    if (!reply.choices.empty()) {
        std::string labels;
        for (const auto& choice : reply.choices) labels += (labels.empty() ? "" : " | ") + choice.label;
        std::printf("     choices: %s   (#1 taps the first)\n", labels.c_str());
    }
    using Expectation = vasdk::flow::Expectation;
    const auto& expect = reply.expectation;
    if (expect.mode() != Expectation::NONE && expect.mode() != Expectation::OPEN) {
        std::string line = Expectation::Mode_Name(expect.mode());
        if (!expect.slot().empty()) line += " " + expect.slot();
        if (expect.keyboard() != Expectation::KEYBOARD_UNSET) line += ", " + Expectation::Keyboard_Name(expect.keyboard()) + " keyboard";
        std::printf("     expects: %s\n", line.c_str());
    }
}

} // namespace

int runAgentTool(int argc, char** argv, std::shared_ptr<const FlowRegistry> registry, const engine::ProviderRegistry* providers) {
    const std::string program = argc > 0 ? std::filesystem::path(argv[0]).filename().string() : "vasdk_agentc";
    auto usage = [&program] { return flow::usage(program); };
    auto load = [&registry](const std::string& path, bool showWarnings) { return flow::load(path, showWarnings, registry); };
    spdlog::set_level(spdlog::level::warn);
    std::vector<std::string> arguments(argv, argv + argc);
#ifdef _WIN32
    // Words with accents ("le 2 août") as typed: the arguments and the console in UTF-8, not the
    // ANSI code page argv and std::cin get by default.
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    int count = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        if (count == argc) {
            for (int i = 0; i < count; ++i) {
                const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
                std::string utf8(size > 0 ? static_cast<size_t>(size - 1) : 0, '\0');
                if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8.data(), size, nullptr, nullptr);
                arguments[static_cast<size_t>(i)] = std::move(utf8);
            }
        }
        LocalFree(wide);
    }
#endif
    std::string flagError;
    const auto completion = takeLlmFlags(arguments, &flagError);
    const auto engineConfig = flagError.empty() ? takeEngineConfig(arguments, &flagError) : std::nullopt;
    if (!flagError.empty()) {
        std::fprintf(stderr, "%s\n", flagError.c_str());
        return 2;
    }
    // The engine config's models: a sentence encoder, an entity model, the understanding settings.
    std::shared_ptr<const nlu::SentenceEncoder> encoder;
    std::shared_ptr<const nlu::EntityExtractor> providedExtractor;   // a provider's (gliner): every agent's
    std::string entityModel;
    vasdk::engine::Understanding understanding;
    if (engineConfig) {
        const engine::ProviderContext context{engineConfig->models_dir().empty() ? modelsNear(arguments[0]) : engineConfig->models_dir(),
                                              {}, 0, nullptr};
        auto provided = [&](const char* what, const vasdk::engine::ProviderSpec& spec, auto made) {
            if (!providers) {
                std::fprintf(stderr, "%s %s: this tool has no providers (an app's own tool, such as garage_agentc, has them)\n", what,
                             spec.id().c_str());
                return decltype(made)(nullptr);
            }
            std::string reason;
            using Interface = typename decltype(made)::element_type;
            auto provider = providers->create<std::remove_const_t<Interface>>(spec, context, &reason);
            if (!provider) std::fprintf(stderr, "%s %s: %s\n", what, spec.id().c_str(), reason.c_str());
            return decltype(made)(provider);
        };
        if (const auto& spec = engineConfig->providers().sentence_encoder(); !spec.id().empty()) {
            encoder = provided("sentence_encoder", spec, encoder);
            if (!encoder) return 1;
        }
        entityModel = engineConfig->providers().entity_model().id();
        if (entityModel == "llm" && !completion) {
            std::fprintf(stderr, "entity_model llm needs --llm\n");
            return 2;
        }
        if (entityModel == "tagger" && !encoder) {
            std::fprintf(stderr, "entity_model tagger needs a sentence_encoder\n");
            return 2;
        }
        if (!entityModel.empty() && entityModel != "llm" && entityModel != "tagger") {
            providedExtractor = provided("entity_model", engineConfig->providers().entity_model(), providedExtractor);
            if (!providedExtractor) return 1;
        }
        understanding = engineConfig->understanding();
    }
    const float minConfidence = understanding.has_min_confidence() ? understanding.min_confidence() : 0.55f;
    const float keywordWords = understanding.has_keyword_words() ? understanding.keyword_words() : 6.0f;
    const float chatMinShareSet = understanding.has_chat_min_share() ? understanding.chat_min_share() : 0.25f;
    std::vector<char*> remaining;
    for (auto& argument : arguments) remaining.push_back(argument.data());
    argc = static_cast<int>(remaining.size());
    argv = remaining.data();
    if (argc < 3) return usage();
    const std::string command = argv[1];
    const std::string agentPath = argv[2];
    // Understanding: the agent's rules (exact with a sentence encoder), the encoder, then the LLM
    // when one is given. Tests and evals wait for its answer (no intent time limit), so results
    // don't depend on how fast the network is.
    auto rulesOf = [&encoder](const std::shared_ptr<const CompiledAgent>& agent) {
        return encoder ? agent->exactRules() : agent->rules();
    };
    auto embeddingsOf = [&encoder](const std::shared_ptr<const CompiledAgent>& agent) -> std::shared_ptr<const nlu::EmbeddingNluProvider> {
        return encoder ? std::make_shared<nlu::EmbeddingNluProvider>(encoder, agent->definition().intents()) : nullptr;
    };
    auto understander = [&](const std::shared_ptr<const CompiledAgent>& agent, std::shared_ptr<const nlu::EmbeddingNluProvider> embeddings,
                            std::shared_ptr<const llm::Completion> asked, float confidence, float keywords) -> std::shared_ptr<const nlu::NluProvider> {
        std::vector<std::shared_ptr<const nlu::NluProvider>> stages{rulesOf(agent)};
        if (embeddings) stages.push_back(embeddings);
        if (asked) stages.push_back(std::make_shared<nlu::LlmNluProvider>(asked, agent->definition().intents(), 0));
        if (stages.size() == 1) return stages.front();
        return std::make_shared<nlu::CascadeNluProvider>(std::move(stages), confidence, keywords);
    };
    auto extractorOf = [&](const std::shared_ptr<const CompiledAgent>& agent) -> std::shared_ptr<const nlu::EntityExtractor> {
        if (entityModel == "llm") return std::make_shared<nlu::LlmEntityExtractor>(completion, 0);
        if (entityModel == "tagger") {
            std::vector<std::string> examples;
            for (const auto& intent : agent->definition().intents())
                examples.insert(examples.end(), intent.examples().begin(), intent.examples().end());
            auto tagger = std::make_shared<nlu::TaggerEntityExtractor>(encoder, agent->entities().modelLabels(), examples);
            return tagger->trained() ? tagger : nullptr;
        }
        return providedExtractor;
    };
    auto stagesText = [&] {
        std::string text = encoder ? "exact rules, " + encoder->name() : "rules";
        if (completion) text += ", llm";
        if (!entityModel.empty()) text += "; entities by " + entityModel;
        return text;
    };
    auto assist = [&](const std::shared_ptr<const CompiledAgent>& agent) -> FlowTestSetup {
        if (!completion && !encoder && entityModel.empty()) return nullptr;
        auto embeddings = embeddingsOf(agent);
        auto nlu = understander(agent, embeddings, completion, minConfidence, keywordWords);
        auto extractor = extractorOf(agent);
        const bool exact = encoder != nullptr;
        const float chatMinShare = chatMinShareSet;
        return [nlu, completion, embeddings, extractor, exact, chatMinShare](FlowRuntime& conversation) {
            conversation.setNlu(nlu);
            if (completion) conversation.setLlm(completion);
            if (embeddings) conversation.setEmbeddings(embeddings);
            if (extractor) conversation.setEntityExtractor(extractor);
            conversation.setExactRules(exact);
            conversation.setChatMinShare(chatMinShare);
        };
    };

    if (command == "lint") {
        auto agent = load(agentPath, true);
        if (!agent) return 1;
        int pages = 0;
        for (const auto& flow : agent->definition().flows()) pages += flow.pages_size();
        std::printf("%s: ok (%d flows, %d pages, %d intents, %d entities)\n", agentPath.c_str(),
                    agent->definition().flows_size(), pages, agent->definition().intents_size(),
                    agent->definition().entity_types_size());
        return 0;
    }
    if (command == "test") {
        if (argc < 4) return usage();
        auto agent = load(agentPath, false);
        if (!agent) return 1;
        int failed = 0;
        const auto setup = assist(agent);
        for (int i = 3; i < argc; ++i) {
            const auto result = va::flow::runFlowTestFile(agent, argv[i], setup);
            std::printf("%s %s\n", result.passed() ? "PASS" : "FAIL", result.name.c_str());
            for (const auto& failure : result.failures) std::printf("    %s\n", failure.c_str());
            if (!result.passed()) ++failed;
        }
        std::printf("%d of %d passed\n", argc - 3 - failed, argc - 3);
        return failed == 0 ? 0 : 1;
    }
    if (command == "eval") {   // eval <agent> <set.txtpb> [--min-accuracy 0.8] [--sweep name=v1,v2]...
        if (argc < 4) return usage();
        auto agent = load(agentPath, false);
        if (!agent) return 1;
        vasdk::flow::NluEvalSet set;
        std::string error;
        if (!va::engine::loadProtoFile(argv[3], set, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        double minAccuracy = 0;
        // The understanding settings (engine config `understanding`), and the values each is tried at.
        std::vector<std::pair<std::string, std::vector<float>>> settings{
            {"min_confidence", {minConfidence}}, {"keyword_words", {keywordWords}}, {"chat_min_share", {chatMinShareSet}}};
        bool swept = false;
        for (int i = 4; i + 1 < argc; i += 2) {
            const std::string flag = argv[i];
            if (flag == "--min-accuracy") {
                minAccuracy = std::atof(argv[i + 1]);
                continue;
            }
            if (flag != "--sweep") return usage();
            const std::string sweep = argv[i + 1];
            const auto equals = sweep.find('=');
            const std::string name = sweep.substr(0, equals);
            auto setting = std::find_if(settings.begin(), settings.end(), [&](const auto& known) { return known.first == name; });
            if (equals == std::string::npos || setting == settings.end()) {
                std::fprintf(stderr, "--sweep %s: min_confidence, keyword_words or chat_min_share=<value>,<value>...\n", sweep.c_str());
                return 2;
            }
            setting->second.clear();
            std::stringstream values(sweep.substr(equals + 1));
            for (std::string value; std::getline(values, value, ',');) setting->second.push_back(static_cast<float>(std::atof(value.c_str())));
            swept = true;
        }
        // An LLM is asked once per sentence, whatever the settings: its answers are kept.
        std::shared_ptr<const llm::Completion> remembered;
        if (completion) {
            struct Remembered : llm::Completion {
                std::shared_ptr<const llm::Completion> inner;
                mutable std::map<std::string, std::optional<std::string>> answers;
                std::optional<std::string> complete(const std::vector<llm::ConversationMessage>& messages) const override {
                    std::string key;
                    for (const auto& message : messages) key += message.content + "\n";
                    const auto found = answers.find(key);
                    if (found != answers.end()) return found->second;
                    return answers[key] = inner->complete(messages);
                }
            };
            auto memo = std::make_shared<Remembered>();
            memo->inner = completion;
            remembered = memo;
        }
        struct Run {
            float minConfidence = 0, keywordWords = 0, chatMinShare = 0;
            int right = 0;
            std::vector<std::string> misses;
        };
        // The entity model's spans of each sentence, found once.
        const auto extractor = extractorOf(agent);
        std::map<std::string, std::vector<nlu::EntitySpan>> spansOf;
        if (extractor)
            for (const auto& item : set.cases())
                spansOf.emplace(item.text(), extractor->extract(entityWords(item.text()), agent->entities().modelLabels(), false));
        auto spansFor = [&](const std::string& text) -> const std::vector<nlu::EntitySpan>* {
            const auto found = spansOf.find(text);
            return found == spansOf.end() ? nullptr : &found->second;
        };
        const auto embeddings = embeddingsOf(agent);   // its vectors are kept across the settings
        auto evaluate = [&](float minConfidence, float keywordWords, float chatMinShare) {
            Run run{minConfidence, keywordWords, chatMinShare};
            const auto nlu = understander(agent, embeddings, remembered, minConfidence, keywordWords);
            for (const auto& item : set.cases()) {
                va::nlu::NluRequest request;
                request.text = item.text();
                request.mentions = [&](const std::string& name) {
                    return agent->entities().find(name, item.text(), {}, std::chrono::system_clock::now(), spansFor(item.text())).has_value();
                };
                // Mid-chat, as FlowRuntime has it: the rules only, and only intents that explain most of the words.
                auto understood = item.mid_chat() ? rulesOf(agent)->understand(request) : nlu->understand(request);
                if (item.mid_chat())
                    understood.intents.erase(std::remove_if(understood.intents.begin(), understood.intents.end(),
                                                            [&](const nlu::IntentMatch& match) { return match.explains < chatMinShare; }),
                                             understood.intents.end());
                const std::string intent = understood.intents.empty() ? std::string() : understood.intents.front().intent;
                if (intent == item.intent()) ++run.right;
                else run.misses.push_back("\"" + item.text() + "\"" + (item.mid_chat() ? " (mid-chat)" : "") + ": " +
                                          (intent.empty() ? "(none)" : intent) + ", expected " + (item.intent().empty() ? "(none)" : item.intent()));
            }
            return run;
        };
        const int total = set.cases_size();
        auto accuracyOf = [total](const Run& run) { return total ? static_cast<double>(run.right) / total : 1.0; };
        std::vector<Run> runs;
        for (const float minConfidence : settings[0].second)
            for (const float keywordWords : settings[1].second)
                for (const float chatMinShare : settings[2].second) runs.push_back(evaluate(minConfidence, keywordWords, chatMinShare));
        const auto best = std::max_element(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.right < b.right; });

        std::map<std::string, std::array<int, 3>> counts;   // entity → right, found, expected
        for (const auto& item : set.cases()) {
            std::map<std::string, std::string> found;
            for (const auto& match : agent->entities().findAll(item.text(), {}, std::chrono::system_clock::now(), spansFor(item.text())))
                found.emplace(match.entity, toText(match.value));
            for (const auto& [name, value] : found) {
                ++counts[name][1];
                const auto expected = item.entities().find(name);
                if (expected != item.entities().end() && expected->second == value) ++counts[name][0];
            }
            for (const auto& [name, value] : item.entities()) ++counts[name][2];
        }
        std::printf("%s: %d utterances, stages %s\n", agentPath.c_str(), total, stagesText().c_str());
        if (swept) {
            if (!completion && (settings[0].second.size() > 1 || settings[1].second.size() > 1))
                std::printf("(min_confidence and keyword_words only matter with a stage after the rules: --llm)\n");
            for (const auto& run : runs)
                std::printf("min_confidence %.2f  keyword_words %.1f  chat_min_share %.2f: %.1f%% (%d of %d)%s\n", run.minConfidence,
                            run.keywordWords, run.chatMinShare, accuracyOf(run) * 100, run.right, total, &run == &*best ? "  best" : "");
        }
        std::printf("intent accuracy: %.1f%% (%d of %d)%s\n", accuracyOf(*best) * 100, best->right, total, swept ? " at the best settings" : "");
        for (const auto& [name, count] : counts) {
            const double precision = count[1] ? static_cast<double>(count[0]) / count[1] : 1.0;
            const double recall = count[2] ? static_cast<double>(count[0]) / count[2] : 1.0;
            const double f1 = precision + recall > 0 ? 2 * precision * recall / (precision + recall) : 0.0;
            std::printf("@%s  precision %.2f  recall %.2f  F1 %.2f\n", name.c_str(), precision, recall, f1);
        }
        for (const auto& miss : best->misses) std::printf("  miss %s\n", miss.c_str());
        return accuracyOf(*best) + 1e-9 >= minAccuracy ? 0 : 1;
    }
    if (command == "try") {
        std::string entity, words;
        for (int i = 3; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--entity" && i + 1 < argc) entity = va::flow::entityName(argv[++i]);
            else words += (words.empty() ? "" : " ") + argument;
        }
        if (words.empty()) return usage();
        auto agent = load(agentPath, false);
        if (!agent) return 1;
        if (!entity.empty()) {
            if (!agent->entities().knows(entity)) {
                std::fprintf(stderr, "unknown entity @%s\n", entity.c_str());
                return 1;
            }
        }
        // Where the entity model finds entities: only there do misspellings count.
        std::optional<std::vector<nlu::EntitySpan>> spans;
        if (const auto extractor = extractorOf(agent)) {
            const auto heard = entityWords(words);
            spans = extractor->extract(heard, agent->entities().modelLabels(), false);
            for (const auto& span : *spans) {
                std::string said;
                for (size_t i = span.start; i < span.start + span.length && i < heard.size(); ++i) said += (said.empty() ? "" : " ") + heard[i];
                std::printf("%s: @%s \"%s\" (%.2f)\n", extractor->name().c_str(), span.entity.c_str(), said.c_str(), span.score);
            }
        }
        const auto* found = spans ? &*spans : nullptr;
        const auto now = std::chrono::system_clock::now();
        if (!entity.empty()) {
            const auto match = agent->entities().find(entity, words, {}, now, found);
            std::printf("%s\n", match ? describe(*match).c_str() : ("@" + entity + ": not found").c_str());
            return match ? 0 : 1;
        }
        // What the rules make of the words, among every intent (pages narrow this down).
        va::nlu::NluRequest request;
        request.text = words;
        request.mentions = [&](const std::string& name) { return agent->entities().find(name, words, {}, now, found).has_value(); };
        const auto understood = understander(agent, embeddingsOf(agent), completion, minConfidence, keywordWords)->understand(request);
        if (understood.intents.empty()) std::printf("intent: (none)\n");
        else std::printf("stage: %s\n", understood.stage.c_str());
        for (size_t i = 0; i < understood.intents.size() && i < 3; ++i) {
            const auto& match = understood.intents[i];
            std::printf("%s %s (score %.2f, confidence %.2f)\n", i == 0 ? "intent:" : "   or:", match.intent.c_str(), match.score,
                        match.confidence);
        }
        for (const auto& match : agent->entities().findAll(words, {}, now, found)) std::printf("%s\n", describe(match).c_str());
        return 0;
    }
    if (command == "chat") {
        auto agent = load(agentPath, false);
        if (!agent) return 1;
        va::flow::FlowRuntime conversation(agent);
        if (const auto setup = assist(agent)) setup(conversation);
        for (int i = 3; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument != "--session" || i + 1 >= argc) return usage();
            const std::string assignment = argv[++i];
            const auto split = assignment.find('=');
            if (split == std::string::npos) return usage();
            // A number, true/false, or else a string; the param's type converts it.
            const std::string text = assignment.substr(split + 1);
            char* end = nullptr;
            const double number = std::strtod(text.c_str(), &end);
            const auto value = text == "true" || text == "false" ? va::flow::boolValue(text == "true")
                             : (!text.empty() && *end == '\0') ? va::flow::numberValue(number)
                             : va::flow::stringValue(text);
            std::string error;
            if (!conversation.setSessionParam(assignment.substr(0, split), value, &error)) {
                std::fprintf(stderr, "--session %s: %s\n", assignment.c_str(), error.c_str());
                return 1;
            }
        }
        print(conversation.start(), conversation);
        std::string line;
        while (std::printf("you: "), std::fflush(stdout), std::getline(std::cin, line)) {
            const bool tap = line.size() > 1 && line[0] == '#' && std::isdigit(static_cast<unsigned char>(line[1]));
            if (line.size() > 1 && line[0] == '!') {   // "!car-ready {"car": "Golf"}": the app fires a trigger
                const auto space = line.find(' ');
                google::protobuf::Struct payload;
                if (space != std::string::npos &&
                    !google::protobuf::util::JsonStringToMessage(line.substr(space + 1), &payload).ok()) {
                    std::printf("(the payload must be a JSON object)\n");
                    continue;
                }
                const auto reply = conversation.fireTrigger(line.substr(1, space == std::string::npos ? std::string::npos : space - 1),
                                                            payload);
                for (const auto& trigger : reply.triggers)
                    std::printf("(trigger %s: %s%s%s)\n", trigger.name.c_str(),
                                vasdk::flow::TriggerDecision_Name(trigger.decision).c_str(), trigger.reason.empty() ? "" : ", ",
                                trigger.reason.c_str());
                if (!reply.text.empty()) print(reply, conversation);
                continue;
            }
            print(line.empty() ? conversation.handleNoInput()
                  : tap        ? conversation.selectChoice(std::atoi(line.c_str() + 1) - 1)
                               : conversation.handleText(line),
                  conversation);
        }
        std::printf("\n");
        return 0;
    }
    return usage();
}

} // namespace flow
} // namespace va
