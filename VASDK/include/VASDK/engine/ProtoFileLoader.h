#pragma once

#include "vasdk_export.h"

#include <string>

namespace google::protobuf { class Message; }
namespace vasdk::engine { class EngineConfig; }

namespace va {
namespace engine {

enum class ProtoFormat { Text, Json, Binary };

/**
 * Inside quoted strings, replaces ${NAME} with the environment variable NAME, and
 * ${NAME:-fallback} with NAME or `fallback` when NAME is unset. Values are escaped for the
 * string they land in; `#` comments are left alone. Fails, naming the variable and its line,
 * when NAME is unset and has no fallback.
 */
VASDK_EXPORT bool expandEnvironmentVariables(const std::string& text, std::string& expanded,
                                             std::string* error = nullptr);

/**
 * Parses `content` into `message`. Unknown fields and type errors fail; text-format errors
 * report "line:column: message" (1-based). `Any` fields resolve against the types linked
 * into the binary, so provider options messages must be compiled in.
 */
VASDK_EXPORT bool parseProto(const std::string& content, ProtoFormat format,
                             google::protobuf::Message& message, std::string* error = nullptr);

/**
 * Loads a .txtpb/.textproto/.pbtxt (text), .json or .pb/.binpb (binary) file into `message`.
 * `expandEnvironment` applies expandEnvironmentVariables() to text and JSON files first.
 */
VASDK_EXPORT bool loadProtoFile(const std::string& path, google::protobuf::Message& message,
                                std::string* error = nullptr, bool expandEnvironment = false);

/** loadProtoFile() for an engine config: ${ENV} references are expanded (secrets live here). */
VASDK_EXPORT bool loadEngineConfig(const std::string& path, vasdk::engine::EngineConfig& config,
                                   std::string* error = nullptr);

} // namespace engine
} // namespace va
