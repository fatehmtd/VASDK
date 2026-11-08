// vasdk: the command installed with the C library. Manages the models local providers load
// (docs/models.md), from the C API through VASDK/c/vasdk.hpp, as any program using the library would.
//
//   vasdk models list                          what the manifest has, and what is downloaded
//   vasdk models pull silero_vad pocket_tts     download (resumes; checks each SHA-256)
//   vasdk models pull --config engine.txtpb     what a config's providers load
//   vasdk models verify|remove <id>...
//   vasdk models path                          where they go (sessions look there when models_dir is unset)
//   vasdk version
//
// Options: --dir <directory> (else VASDK_MODELS_DIR, else the user cache), --offline,
// --accept-license (for a model whose license needs reading first), --json (list), --manifest
// <file> (another manifest than the built-in one: a mirror, models of your own).

#include <VASDK/c/vasdk.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: vasdk models list [--json]\n"
                 "       vasdk models pull <id>... | --config <engine config> [--accept-license]\n"
                 "       vasdk models verify <id>...\n"
                 "       vasdk models remove <id>...\n"
                 "       vasdk models path\n"
                 "       vasdk version\n"
                 "options: --dir <directory>  --manifest <file>  --offline\n");
    return 2;
}

std::string megabytes(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(bytes) / 1e6);
    return text;
}

struct Arguments {
    std::vector<std::string> positional;
    std::string directory;
    std::string config;
    std::string manifest;
    bool offline = false;
    bool acceptLicense = false;
    bool json = false;
};

bool parse(int argc, char** argv, Arguments& arguments) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if ((argument == "--dir" || argument == "--config" || argument == "--manifest") && index + 1 < argc) {
            (argument == "--dir" ? arguments.directory : argument == "--config" ? arguments.config : arguments.manifest) = argv[++index];
        } else if (argument == "--offline") {
            arguments.offline = true;
        } else if (argument == "--accept-license") {
            arguments.acceptLicense = true;
        } else if (argument == "--json") {
            arguments.json = true;
        } else if (argument.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option %s\n", argument.c_str());
            return false;
        } else {
            arguments.positional.push_back(argument);
        }
    }
    return true;
}

int list(const vasdk::Models& models, bool json) {
    const std::string listed = models.list();
    if (json) {
        std::printf("%s\n", listed.c_str());
        return 0;
    }
    std::printf("%-22s %-8s %9s  %-22s %s\n", "MODEL", "STATUS", "SIZE", "LICENSE", "PROVIDERS");
    for (const auto& model : nlohmann::json::parse(listed)) {
        std::string providers;
        for (const auto& provider : model["providers"]) providers += (providers.empty() ? "" : ", ") + provider.get<std::string>();
        std::string license = model["license"].get<std::string>();
        if (model["requires_acceptance"].get<bool>()) license += " *";
        std::printf("%-22s %-8s %9s  %-22s %s\n", model["id"].get<std::string>().c_str(), model["status"].get<std::string>().c_str(),
                    megabytes(model["size"].get<std::uint64_t>()).c_str(), license.c_str(), providers.c_str());
    }
    std::printf("\n* read its license, then pull it with --accept-license.  Directory: %s\n", models.directory().c_str());
    return 0;
}

int pull(const vasdk::Models& models, const Arguments& arguments) {
    std::vector<std::string> ids(arguments.positional.begin() + 2, arguments.positional.end());
    if (!arguments.config.empty()) {
        const auto needed = models.forConfig(arguments.config);
        ids.insert(ids.end(), needed.begin(), needed.end());
        if (needed.empty()) std::printf("%s names no provider with a model to download\n", arguments.config.c_str());
    }
    int failed = 0;
    for (const auto& id : ids) {
        int lastPercent = -1;
        try {
            models.pull(id, arguments.acceptLicense, [&](std::string_view model, std::string_view, std::uint64_t done, std::uint64_t total) {
                const int percent = total ? static_cast<int>(done * 100 / total) : 100;
                if (percent != lastPercent) {
                    std::printf("\r%.*s: %3d%% of %s", static_cast<int>(model.size()), model.data(), percent, megabytes(total).c_str());
                    std::fflush(stdout);
                    lastPercent = percent;
                }
                return true;
            });
            std::printf("\r%s: present%30s\n", id.c_str(), "");
        } catch (const vasdk::Error& error) {
            std::printf("%s", lastPercent >= 0 ? "\n" : "");
            std::fprintf(stderr, "%s\n", error.what());
            ++failed;
        }
    }
    return failed == 0 ? 0 : 1;
}

int each(const vasdk::Models& models, const Arguments& arguments, bool verify) {
    int failed = 0;
    for (std::size_t index = 2; index < arguments.positional.size(); ++index) {
        const std::string& id = arguments.positional[index];
        try {
            if (verify) models.verify(id);
            else models.remove(id);
            std::printf("%s: %s\n", id.c_str(), verify ? "verified" : "removed");
        } catch (const vasdk::Error& error) {
            std::fprintf(stderr, "%s\n", error.what());
            ++failed;
        }
    }
    return failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    if (!parse(argc, argv, arguments) || arguments.positional.empty()) return usage();
    const std::string& command = arguments.positional[0];
    if (command == "version") {
        std::printf("VASDK %s (C API %d)\n", vasdk::version().c_str(), vasdk::apiVersion());
        return 0;
    }
    if (command != "models" || arguments.positional.size() < 2) return usage();

    nlohmann::json options = nlohmann::json::object();
    if (!arguments.directory.empty()) options["directory"] = arguments.directory;
    if (!arguments.manifest.empty()) options["manifest_path"] = arguments.manifest;
    if (arguments.offline) options["offline"] = true;
    try {
        const vasdk::Models models(options.empty() ? std::string() : options.dump());
        const std::string& action = arguments.positional[1];
        if (action == "list") return list(models, arguments.json);
        if (action == "path") {
            std::printf("%s\n", models.directory().c_str());
            return 0;
        }
        if (action == "pull" && (arguments.positional.size() > 2 || !arguments.config.empty())) return pull(models, arguments);
        if ((action == "verify" || action == "remove") && arguments.positional.size() > 2) return each(models, arguments, action == "verify");
    } catch (const vasdk::Error& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return usage();
}
