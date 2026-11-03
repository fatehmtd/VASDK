#include <VASDK/engine/ProtoFileLoader.h>

#include <vasdk/engine/engine_config.pb.h>

#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace va {
namespace engine {

namespace {

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

int lineAt(const std::string& text, size_t position) {
    return 1 + static_cast<int>(std::count(text.begin(), text.begin() + position, '\n'));
}

class TextErrorCollector : public google::protobuf::io::ErrorCollector {
public:
    void RecordError(int line, google::protobuf::io::ColumnNumber column,
                     absl::string_view message) override {
        if (!errors.empty()) errors += "\n";
        errors += std::to_string(line + 1) + ":" + std::to_string(column + 1) + ": " + std::string(message);
    }
    std::string errors;
};

bool formatForPath(const std::string& path, ProtoFormat& format) {
    auto endsWith = [&](const char* suffix) {
        std::string s(suffix);
        return path.size() >= s.size() && path.compare(path.size() - s.size(), s.size(), s) == 0;
    };
    if (endsWith(".txtpb") || endsWith(".textproto") || endsWith(".pbtxt")) format = ProtoFormat::Text;
    else if (endsWith(".json")) format = ProtoFormat::Json;
    else if (endsWith(".pb") || endsWith(".binpb")) format = ProtoFormat::Binary;
    else return false;
    return true;
}

} // namespace

bool expandEnvironmentVariables(const std::string& text, std::string& expanded, std::string* error) {
    std::string out;
    out.reserve(text.size());
    char quote = 0;   // quote character of the open string literal, 0 outside one
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (!quote) {
            if (c == '#') {   // text-proto comment: copied as is, never expanded
                size_t end = text.find('\n', i);
                if (end == std::string::npos) end = text.size();
                out.append(text, i, end - i);
                i = end - 1;
                continue;
            }
            if (c == '"' || c == '\'') quote = c;
            out += c;
            continue;
        }
        if (c == '\\' && i + 1 < text.size()) { out += c; out += text[++i]; continue; }
        if (c == quote) { quote = 0; out += c; continue; }
        if (c != '$' || i + 1 >= text.size() || text[i + 1] != '{') { out += c; continue; }

        size_t end = text.find('}', i + 2);
        if (end == std::string::npos)
            return fail(error, "line " + std::to_string(lineAt(text, i)) + ": unterminated ${");
        std::string reference = text.substr(i + 2, end - i - 2);
        size_t separator = reference.find(":-");
        std::string name = reference.substr(0, separator);
        std::string value;
        if (const char* environment = std::getenv(name.c_str())) {
            value = environment;
        } else if (separator != std::string::npos) {
            value = reference.substr(separator + 2);
        } else {
            return fail(error, "line " + std::to_string(lineAt(text, i)) + ": environment variable "
                               + name + " is not set (use ${" + name + ":-} for an optional value)");
        }
        // Escaped so a value cannot end the string literal it is substituted into.
        for (char v : value) {
            if (v == '\n') { out += "\\n"; continue; }
            if (v == '\\' || v == quote) out += '\\';
            out += v;
        }
        i = end;
    }
    expanded = std::move(out);
    return true;
}

bool parseProto(const std::string& content, ProtoFormat format,
                google::protobuf::Message& message, std::string* error) {
    message.Clear();
    switch (format) {
    case ProtoFormat::Text: {
        TextErrorCollector collector;
        google::protobuf::TextFormat::Parser parser;
        parser.RecordErrorsTo(&collector);
        if (!parser.ParseFromString(content, &message)) return fail(error, collector.errors);
        return true;
    }
    case ProtoFormat::Json: {
        auto status = google::protobuf::util::JsonStringToMessage(content, &message);
        if (!status.ok()) return fail(error, std::string(status.message()));
        return true;
    }
    case ProtoFormat::Binary:
        if (!message.ParseFromString(content)) return fail(error, "not a valid binary " + std::string(message.GetTypeName()));
        return true;
    }
    return fail(error, "unknown format");
}

bool loadProtoFile(const std::string& path, google::protobuf::Message& message,
                   std::string* error, bool expandEnvironment) {
    ProtoFormat format;
    if (!formatForPath(path, format))
        return fail(error, path + ": unknown extension (use .txtpb, .json or .pb)");

    std::ifstream file(path, std::ios::binary);
    if (!file) return fail(error, path + ": cannot open");
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    std::string detail;
    if (expandEnvironment && format != ProtoFormat::Binary
        && !expandEnvironmentVariables(content, content, &detail))
        return fail(error, path + ": " + detail);
    if (!parseProto(content, format, message, &detail))
        return fail(error, path + ":" + (format == ProtoFormat::Text ? "" : " ") + detail);
    return true;
}

bool loadEngineConfig(const std::string& path, vasdk::engine::EngineConfig& config, std::string* error) {
    return loadProtoFile(path, config, error, /*expandEnvironment=*/true);
}

} // namespace engine
} // namespace va
